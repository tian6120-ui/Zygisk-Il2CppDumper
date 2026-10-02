#include "AZLua.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "Il2cpp/Il2cpp.h"
#include "Il2cpp/il2cpp-class.h"
#include "imgui/imgui.h"

extern "C" {
#include "Lua/lua.h"
#include "Lua/lauxlib.h"
#include "Lua/lualib.h"
}

namespace {

constexpr const char* MT_CLASS = "AZ.Class";
constexpr const char* MT_INSTANCE = "AZ.Instance";

struct LuaClass {
    Il2CppClass* klass{};
};

struct LuaInstance {
    uint32_t handle{};
};

lua_State* g_L = nullptr;
std::mutex g_luaMutex;
std::mutex g_outputMutex;
std::vector<std::string> g_output;

// IL2CPP exports resolved by the upstream core. We use these for metadata-safe
// array element writes rather than assuming a Unity version's array layout.
extern uint32_t (*il2cpp_array_object_header_size)();
extern int (*il2cpp_class_array_element_size)(Il2CppClass*);
extern void (*il2cpp_gc_wbarrier_set_field)(Il2CppObject*, void**, void*);

enum class RetainedKind {
    Text,
    Separator,
    SameLine,
    Button,
    Checkbox,
    InputText,
    InputInt,
    InputFloat,
    SliderInt,
    Combo
};

struct RetainedWidget {
    RetainedKind kind{RetainedKind::Text};
    std::string id;
    std::string label;
    std::string text;
    bool boolValue{};
    int intValue{};
    int minValue{};
    int maxValue{100};
    float floatValue{};
    std::vector<std::string> options;
    int callbackRef{LUA_NOREF};
};

struct RetainedWindow {
    std::string id;
    std::string title;
    bool open{true};
    std::vector<RetainedWidget> widgets;
};

std::mutex g_uiMutex;
std::unordered_map<std::string, RetainedWindow> g_windows;
char g_scriptName[128] = "script.lua";
std::string g_selectedScript;
bool g_alertPending = false;
std::string g_alertText;

char g_editor[64 * 1024] =
    "-- AZ Tool Lua 5.4.7\n"
    "-- Example:\n"
    "-- local c = Class.fromName(\"UnityEngine.Application\")\n"
    "-- print(c)\n";

void appendOutput(const std::string& s) {
    std::lock_guard<std::mutex> lock(g_outputMutex);
    g_output.push_back(s);
    constexpr size_t kMaxLines = 1000;
    if (g_output.size() > kMaxLines) {
        g_output.erase(g_output.begin(), g_output.begin() + (g_output.size() - kMaxLines));
    }
}

std::string className(Il2CppClass* klass) {
    if (!klass) return "<null class>";
    const char* ns = Il2cpp::GetClassNamespace(klass);
    const char* n = Il2cpp::GetClassName(klass);
    std::string out;
    if (ns && *ns) {
        out += ns;
        out += ".";
    }
    out += n ? n : "?";
    return out;
}

LuaClass* checkClass(lua_State* L, int idx) {
    return static_cast<LuaClass*>(luaL_checkudata(L, idx, MT_CLASS));
}

LuaInstance* checkInstance(lua_State* L, int idx) {
    return static_cast<LuaInstance*>(luaL_checkudata(L, idx, MT_INSTANCE));
}

Il2CppObject* getInstanceObject(LuaInstance* u) {
    if (!u || !u->handle) return nullptr;
    return Il2cpp::GC::GetHandleTarget(u->handle);
}

int pushClass(lua_State* L, Il2CppClass* klass) {
    if (!klass) {
        lua_pushnil(L);
        return 1;
    }
    auto* u = static_cast<LuaClass*>(lua_newuserdatauv(L, sizeof(LuaClass), 0));
    u->klass = klass;
    luaL_getmetatable(L, MT_CLASS);
    lua_setmetatable(L, -2);
    return 1;
}

int pushInstance(lua_State* L, Il2CppObject* obj) {
    if (!obj) {
        lua_pushnil(L);
        return 1;
    }
    const uint32_t handle = Il2cpp::GC::NewHandle(obj);
    if (!handle) {
        return luaL_error(L, "AZ: could not create GC handle for managed object");
    }
    auto* u = static_cast<LuaInstance*>(lua_newuserdatauv(L, sizeof(LuaInstance), 0));
    u->handle = handle;
    luaL_getmetatable(L, MT_INSTANCE);
    lua_setmetatable(L, -2);
    return 1;
}

int l_instance_gc(lua_State* L) {
    auto* u = static_cast<LuaInstance*>(luaL_testudata(L, 1, MT_INSTANCE));
    if (u && u->handle) {
        Il2cpp::GC::FreeHandle(u->handle);
        u->handle = 0;
    }
    return 0;
}

int l_class_tostring(lua_State* L) {
    auto* u = checkClass(L, 1);
    std::string s = "Class<" + className(u->klass) + ">";
    lua_pushlstring(L, s.c_str(), s.size());
    return 1;
}

int l_instance_tostring(lua_State* L) {
    auto* u = checkInstance(L, 1);
    auto* obj = getInstanceObject(u);
    if (!obj) {
        lua_pushliteral(L, "Instance<stale>");
        return 1;
    }
    std::ostringstream oss;
    oss << "Instance<" << className(Il2cpp::GetObjectClass(obj)) << ">@0x"
        << std::hex << reinterpret_cast<uintptr_t>(obj);
    const auto s = oss.str();
    lua_pushlstring(L, s.c_str(), s.size());
    return 1;
}

int l_print(lua_State* L) {
    const int n = lua_gettop(L);
    std::ostringstream oss;
    for (int i = 1; i <= n; ++i) {
        size_t len = 0;
        const char* s = luaL_tolstring(L, i, &len);
        if (i > 1) oss << "\t";
        if (s) oss.write(s, static_cast<std::streamsize>(len));
        lua_pop(L, 1);
    }
    appendOutput(oss.str());
    return 0;
}

int l_class_from_name(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    if (!Il2cpp::ApiResolved() && !Il2cpp::Init()) {
        return luaL_error(L, "AZ: IL2CPP API not ready");
    }
    Il2CppClass* klass = Il2cpp::FindClass(name);
    if (!klass) {
        lua_pushnil(L);
        return 1;
    }
    return pushClass(L, klass);
}

int l_class_find_objects(lua_State* L) {
    auto* c = checkClass(L, 1);
    if (!c->klass) {
        lua_newtable(L);
        return 1;
    }
    auto objects = Il2cpp::GC::FindObjects(c->klass);
    lua_createtable(L, static_cast<int>(objects.size()), 0);
    int outIndex = 1;
    for (auto* obj : objects) {
        if (!obj) continue;
        pushInstance(L, obj);
        lua_rawseti(L, -2, outIndex++);
    }
    return 1;
}

int l_class_fields(lua_State* L) {
    auto* c = checkClass(L, 1);
    lua_newtable(L);
    if (!c->klass) return 1;

    void* iter = nullptr;
    int index = 1;
    while (auto* f = Il2cpp::GetClassFields(c->klass, &iter)) {
        lua_newtable(L);

        lua_pushstring(L, Il2cpp::GetFieldName(f) ? Il2cpp::GetFieldName(f) : "?");
        lua_setfield(L, -2, "name");

        auto* t = Il2cpp::GetFieldType(f);
        lua_pushstring(L, t ? Il2cpp::GetTypeName(t) : "?");
        lua_setfield(L, -2, "type");

        lua_pushinteger(L, static_cast<lua_Integer>(Il2cpp::GetFieldOffset(f)));
        lua_setfield(L, -2, "offset");

        lua_pushinteger(L, static_cast<lua_Integer>(Il2cpp::GetFieldFlags(f)));
        lua_setfield(L, -2, "flags");

        lua_rawseti(L, -2, index++);
    }
    return 1;
}

int pushValue(lua_State* L, Il2CppType* type, Il2CppObject* boxedOrObject) {
    if (!type) {
        if (boxedOrObject) return pushInstance(L, boxedOrObject);
        lua_pushnil(L);
        return 1;
    }

    if (!boxedOrObject) {
        lua_pushnil(L);
        return 1;
    }

    void* raw = Il2cpp::GetUnboxedValue(boxedOrObject);
    switch (type->type) {
        case IL2CPP_TYPE_VOID:
            lua_pushnil(L);
            return 1;
        case IL2CPP_TYPE_BOOLEAN:
            lua_pushboolean(L, raw ? (*static_cast<uint8_t*>(raw) != 0) : 0);
            return 1;
        case IL2CPP_TYPE_I1:
            lua_pushinteger(L, raw ? *static_cast<int8_t*>(raw) : 0);
            return 1;
        case IL2CPP_TYPE_U1:
            lua_pushinteger(L, raw ? *static_cast<uint8_t*>(raw) : 0);
            return 1;
        case IL2CPP_TYPE_I2:
            lua_pushinteger(L, raw ? *static_cast<int16_t*>(raw) : 0);
            return 1;
        case IL2CPP_TYPE_U2:
        case IL2CPP_TYPE_CHAR:
            lua_pushinteger(L, raw ? *static_cast<uint16_t*>(raw) : 0);
            return 1;
        case IL2CPP_TYPE_I4:
            lua_pushinteger(L, raw ? *static_cast<int32_t*>(raw) : 0);
            return 1;
        case IL2CPP_TYPE_U4:
            lua_pushinteger(L, raw ? *static_cast<uint32_t*>(raw) : 0);
            return 1;
        case IL2CPP_TYPE_I8:
            lua_pushinteger(L, raw ? static_cast<lua_Integer>(*static_cast<int64_t*>(raw)) : 0);
            return 1;
        case IL2CPP_TYPE_U8:
            lua_pushinteger(L, raw ? static_cast<lua_Integer>(*static_cast<uint64_t*>(raw)) : 0);
            return 1;
        case IL2CPP_TYPE_R4:
            lua_pushnumber(L, raw ? static_cast<lua_Number>(*static_cast<float*>(raw)) : 0.0);
            return 1;
        case IL2CPP_TYPE_R8:
            lua_pushnumber(L, raw ? static_cast<lua_Number>(*static_cast<double*>(raw)) : 0.0);
            return 1;
        case IL2CPP_TYPE_STRING: {
            auto* s = reinterpret_cast<Il2CppString*>(boxedOrObject);
            const std::string v = s ? s->to_string() : std::string();
            lua_pushlstring(L, v.c_str(), v.size());
            return 1;
        }
        case IL2CPP_TYPE_CLASS:
        case IL2CPP_TYPE_OBJECT:
        case IL2CPP_TYPE_SZARRAY:
        case IL2CPP_TYPE_ARRAY:
        case IL2CPP_TYPE_GENERICINST:
            return pushInstance(L, boxedOrObject);
        default:
            return pushInstance(L, boxedOrObject);
    }
}

int pushFieldValue(lua_State* L, FieldInfo* f, Il2CppObject* obj, bool isStatic) {
    if (!f) {
        lua_pushnil(L);
        return 1;
    }
    Il2CppType* type = Il2cpp::GetFieldType(f);
    if (!type) {
        lua_pushnil(L);
        return 1;
    }

    switch (type->type) {
        case IL2CPP_TYPE_BOOLEAN: {
            uint8_t v{};
            if (isStatic) Il2cpp::GetFieldStaticValue(f, &v);
            else Il2cpp::GetFieldValue(obj, f, &v);
            lua_pushboolean(L, v != 0);
            return 1;
        }
        case IL2CPP_TYPE_I1: {
            int8_t v{};
            if (isStatic) Il2cpp::GetFieldStaticValue(f, &v);
            else Il2cpp::GetFieldValue(obj, f, &v);
            lua_pushinteger(L, v);
            return 1;
        }
        case IL2CPP_TYPE_U1: {
            uint8_t v{};
            if (isStatic) Il2cpp::GetFieldStaticValue(f, &v);
            else Il2cpp::GetFieldValue(obj, f, &v);
            lua_pushinteger(L, v);
            return 1;
        }
        case IL2CPP_TYPE_I2: {
            int16_t v{};
            if (isStatic) Il2cpp::GetFieldStaticValue(f, &v);
            else Il2cpp::GetFieldValue(obj, f, &v);
            lua_pushinteger(L, v);
            return 1;
        }
        case IL2CPP_TYPE_U2:
        case IL2CPP_TYPE_CHAR: {
            uint16_t v{};
            if (isStatic) Il2cpp::GetFieldStaticValue(f, &v);
            else Il2cpp::GetFieldValue(obj, f, &v);
            lua_pushinteger(L, v);
            return 1;
        }
        case IL2CPP_TYPE_I4: {
            int32_t v{};
            if (isStatic) Il2cpp::GetFieldStaticValue(f, &v);
            else Il2cpp::GetFieldValue(obj, f, &v);
            lua_pushinteger(L, v);
            return 1;
        }
        case IL2CPP_TYPE_U4: {
            uint32_t v{};
            if (isStatic) Il2cpp::GetFieldStaticValue(f, &v);
            else Il2cpp::GetFieldValue(obj, f, &v);
            lua_pushinteger(L, static_cast<lua_Integer>(v));
            return 1;
        }
        case IL2CPP_TYPE_I8: {
            int64_t v{};
            if (isStatic) Il2cpp::GetFieldStaticValue(f, &v);
            else Il2cpp::GetFieldValue(obj, f, &v);
            lua_pushinteger(L, static_cast<lua_Integer>(v));
            return 1;
        }
        case IL2CPP_TYPE_U8: {
            uint64_t v{};
            if (isStatic) Il2cpp::GetFieldStaticValue(f, &v);
            else Il2cpp::GetFieldValue(obj, f, &v);
            lua_pushinteger(L, static_cast<lua_Integer>(v));
            return 1;
        }
        case IL2CPP_TYPE_R4: {
            float v{};
            if (isStatic) Il2cpp::GetFieldStaticValue(f, &v);
            else Il2cpp::GetFieldValue(obj, f, &v);
            lua_pushnumber(L, v);
            return 1;
        }
        case IL2CPP_TYPE_R8: {
            double v{};
            if (isStatic) Il2cpp::GetFieldStaticValue(f, &v);
            else Il2cpp::GetFieldValue(obj, f, &v);
            lua_pushnumber(L, v);
            return 1;
        }
        case IL2CPP_TYPE_STRING:
        case IL2CPP_TYPE_CLASS:
        case IL2CPP_TYPE_OBJECT:
        case IL2CPP_TYPE_SZARRAY:
        case IL2CPP_TYPE_ARRAY:
        case IL2CPP_TYPE_GENERICINST: {
            Il2CppObject* v = isStatic
                ? Il2cpp::GetFieldValueObject(nullptr, f)
                : Il2cpp::GetFieldValueObject(obj, f);
            if (type->type == IL2CPP_TYPE_STRING) {
                auto* s = reinterpret_cast<Il2CppString*>(v);
                if (!s) {
                    lua_pushnil(L);
                    return 1;
                }
                std::string str = s->to_string();
                lua_pushlstring(L, str.c_str(), str.size());
                return 1;
            }
            return pushInstance(L, v);
        }
        default:
            lua_pushnil(L);
            return 1;
    }
}

bool setFieldFromLua(lua_State* L, FieldInfo* f, Il2CppObject* obj, bool isStatic, int valueIndex) {
    if (!f) return false;
    Il2CppType* type = Il2cpp::GetFieldType(f);
    if (!type) return false;

    auto setRaw = [&](void* p) {
        if (isStatic) Il2cpp::SetFieldStaticValue(f, p);
        else Il2cpp::SetFieldValue(obj, f, p);
    };

    switch (type->type) {
        case IL2CPP_TYPE_BOOLEAN: {
            uint8_t v = lua_toboolean(L, valueIndex) ? 1 : 0;
            setRaw(&v);
            return true;
        }
        case IL2CPP_TYPE_I1: {
            int8_t v = static_cast<int8_t>(luaL_checkinteger(L, valueIndex));
            setRaw(&v);
            return true;
        }
        case IL2CPP_TYPE_U1: {
            uint8_t v = static_cast<uint8_t>(luaL_checkinteger(L, valueIndex));
            setRaw(&v);
            return true;
        }
        case IL2CPP_TYPE_I2: {
            int16_t v = static_cast<int16_t>(luaL_checkinteger(L, valueIndex));
            setRaw(&v);
            return true;
        }
        case IL2CPP_TYPE_U2:
        case IL2CPP_TYPE_CHAR: {
            uint16_t v = static_cast<uint16_t>(luaL_checkinteger(L, valueIndex));
            setRaw(&v);
            return true;
        }
        case IL2CPP_TYPE_I4: {
            int32_t v = static_cast<int32_t>(luaL_checkinteger(L, valueIndex));
            setRaw(&v);
            return true;
        }
        case IL2CPP_TYPE_U4: {
            uint32_t v = static_cast<uint32_t>(luaL_checkinteger(L, valueIndex));
            setRaw(&v);
            return true;
        }
        case IL2CPP_TYPE_I8: {
            int64_t v = static_cast<int64_t>(luaL_checkinteger(L, valueIndex));
            setRaw(&v);
            return true;
        }
        case IL2CPP_TYPE_U8: {
            uint64_t v = static_cast<uint64_t>(luaL_checkinteger(L, valueIndex));
            setRaw(&v);
            return true;
        }
        case IL2CPP_TYPE_R4: {
            float v = static_cast<float>(luaL_checknumber(L, valueIndex));
            setRaw(&v);
            return true;
        }
        case IL2CPP_TYPE_R8: {
            double v = static_cast<double>(luaL_checknumber(L, valueIndex));
            setRaw(&v);
            return true;
        }
        case IL2CPP_TYPE_STRING: {
            const char* s = luaL_checkstring(L, valueIndex);
            Il2CppString* managed = Il2cpp::NewString(s);
            Il2cpp::SetFieldValueObject(obj, f, reinterpret_cast<Il2CppObject*>(managed));
            return true;
        }
        case IL2CPP_TYPE_CLASS:
        case IL2CPP_TYPE_OBJECT:
        case IL2CPP_TYPE_SZARRAY:
        case IL2CPP_TYPE_ARRAY:
        case IL2CPP_TYPE_GENERICINST: {
            Il2CppObject* managed = nullptr;
            if (!lua_isnil(L, valueIndex)) {
                auto* inst = static_cast<LuaInstance*>(luaL_testudata(L, valueIndex, MT_INSTANCE));
                if (!inst) return false;
                managed = getInstanceObject(inst);
                if (!managed) return false;
            }
            Il2cpp::SetFieldValueObject(obj, f, managed);
            return true;
        }
        default:
            return false;
    }
}

int l_class_get_static(lua_State* L) {
    auto* c = checkClass(L, 1);
    const char* name = luaL_checkstring(L, 2);
    auto* f = c->klass ? c->klass->getFieldInHierarchy(name) : nullptr;
    if (!f) {
        lua_pushnil(L);
        return 1;
    }
    return pushFieldValue(L, f, nullptr, true);
}

int l_class_set_field(lua_State* L) {
    auto* c = checkClass(L, 1);
    const char* name = luaL_checkstring(L, 2);
    auto* f = c->klass ? c->klass->getFieldInHierarchy(name) : nullptr;
    if (!f) return luaL_error(L, "AZ: static field not found: %s", name);
    if (!setFieldFromLua(L, f, nullptr, true, 3)) {
        return luaL_error(L, "AZ: unsupported static field type: %s", name);
    }
    lua_pushboolean(L, 1);
    return 1;
}

int l_instance_get_address(lua_State* L) {
    auto* u = checkInstance(L, 1);
    auto* obj = getInstanceObject(u);
    lua_pushinteger(L, static_cast<lua_Integer>(reinterpret_cast<uintptr_t>(obj)));
    return 1;
}

int l_instance_get_field(lua_State* L) {
    auto* u = checkInstance(L, 1);
    auto* obj = getInstanceObject(u);
    if (!obj) return luaL_error(L, "AZ: selected object is no longer alive");
    const char* name = luaL_checkstring(L, 2);
    auto* klass = Il2cpp::GetObjectClass(obj);
    auto* f = klass ? klass->getFieldInHierarchy(name) : nullptr;
    if (!f) {
        lua_pushnil(L);
        return 1;
    }
    return pushFieldValue(L, f, obj, false);
}

int l_instance_set_field(lua_State* L) {
    auto* u = checkInstance(L, 1);
    auto* obj = getInstanceObject(u);
    if (!obj) return luaL_error(L, "AZ: selected object is no longer alive");
    const char* name = luaL_checkstring(L, 2);
    auto* klass = Il2cpp::GetObjectClass(obj);
    auto* f = klass ? klass->getFieldInHierarchy(name) : nullptr;
    if (!f) return luaL_error(L, "AZ: field not found: %s", name);
    if (!setFieldFromLua(L, f, obj, false, 3)) {
        return luaL_error(L, "AZ: unsupported field type: %s", name);
    }
    lua_pushboolean(L, 1);
    return 1;
}

Il2CppObject* boxLuaValue(lua_State* L, int idx, Il2CppType* type) {
    if (!type) return nullptr;
    if (lua_isnil(L, idx)) return nullptr;

    switch (type->type) {
        case IL2CPP_TYPE_BOOLEAN: {
            uint8_t v = lua_toboolean(L, idx) ? 1 : 0;
            return Il2cpp::GetBoxedValue(type->getClass(), &v);
        }
        case IL2CPP_TYPE_I1: {
            int8_t v = static_cast<int8_t>(luaL_checkinteger(L, idx));
            return Il2cpp::GetBoxedValue(type->getClass(), &v);
        }
        case IL2CPP_TYPE_U1: {
            uint8_t v = static_cast<uint8_t>(luaL_checkinteger(L, idx));
            return Il2cpp::GetBoxedValue(type->getClass(), &v);
        }
        case IL2CPP_TYPE_I2: {
            int16_t v = static_cast<int16_t>(luaL_checkinteger(L, idx));
            return Il2cpp::GetBoxedValue(type->getClass(), &v);
        }
        case IL2CPP_TYPE_U2:
        case IL2CPP_TYPE_CHAR: {
            uint16_t v = static_cast<uint16_t>(luaL_checkinteger(L, idx));
            return Il2cpp::GetBoxedValue(type->getClass(), &v);
        }
        case IL2CPP_TYPE_I4: {
            int32_t v = static_cast<int32_t>(luaL_checkinteger(L, idx));
            return Il2cpp::GetBoxedValue(type->getClass(), &v);
        }
        case IL2CPP_TYPE_U4: {
            uint32_t v = static_cast<uint32_t>(luaL_checkinteger(L, idx));
            return Il2cpp::GetBoxedValue(type->getClass(), &v);
        }
        case IL2CPP_TYPE_I8: {
            int64_t v = static_cast<int64_t>(luaL_checkinteger(L, idx));
            return Il2cpp::GetBoxedValue(type->getClass(), &v);
        }
        case IL2CPP_TYPE_U8: {
            uint64_t v = static_cast<uint64_t>(luaL_checkinteger(L, idx));
            return Il2cpp::GetBoxedValue(type->getClass(), &v);
        }
        case IL2CPP_TYPE_R4: {
            float v = static_cast<float>(luaL_checknumber(L, idx));
            return Il2cpp::GetBoxedValue(type->getClass(), &v);
        }
        case IL2CPP_TYPE_R8: {
            double v = static_cast<double>(luaL_checknumber(L, idx));
            return Il2cpp::GetBoxedValue(type->getClass(), &v);
        }
        case IL2CPP_TYPE_STRING:
            return reinterpret_cast<Il2CppObject*>(Il2cpp::NewString(luaL_checkstring(L, idx)));
        case IL2CPP_TYPE_CLASS:
        case IL2CPP_TYPE_OBJECT:
        case IL2CPP_TYPE_SZARRAY:
        case IL2CPP_TYPE_ARRAY:
        case IL2CPP_TYPE_GENERICINST: {
            auto* inst = static_cast<LuaInstance*>(luaL_testudata(L, idx, MT_INSTANCE));
            return inst ? getInstanceObject(inst) : nullptr;
        }
        default:
            return nullptr;
    }
}

int invokeMethod(lua_State* L, MethodInfo* method, Il2CppObject* instance, int firstArg) {
    if (!method) return luaL_error(L, "AZ: method not found");

    const uint32_t count = Il2cpp::GetMethodParamCount(method);
    const int supplied = lua_gettop(L) - firstArg + 1;
    if (supplied != static_cast<int>(count)) {
        return luaL_error(L, "AZ: argument count mismatch, expected %u got %d", count, supplied);
    }

    std::vector<Il2CppObject*> args;
    args.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        auto* t = Il2cpp::GetMethodParam(method, i);
        Il2CppObject* boxed = boxLuaValue(L, firstArg + static_cast<int>(i), t);
        if (!boxed && !lua_isnil(L, firstArg + static_cast<int>(i)) &&
            t && t->type != IL2CPP_TYPE_CLASS && t->type != IL2CPP_TYPE_OBJECT &&
            t->type != IL2CPP_TYPE_ARRAY && t->type != IL2CPP_TYPE_SZARRAY &&
            t->type != IL2CPP_TYPE_GENERICINST) {
            return luaL_error(L, "AZ: unsupported argument type at index %u: %s",
                              i + 1, t ? Il2cpp::GetTypeName(t) : "?");
        }
        args.push_back(boxed);
    }

    Il2CppException* exc = nullptr;
    Il2CppObject* result = Il2cpp::RuntimeInvokeConvertArgs(
        method, instance, args.empty() ? nullptr : args.data(),
        static_cast<int>(args.size()), &exc);

    if (exc) {
        return luaL_error(L, "AZ: managed exception in %s", Il2cpp::GetMethodName(method));
    }

    auto* ret = Il2cpp::GetMethodReturnType(method);
    if (ret && ret->type == IL2CPP_TYPE_VOID) {
        lua_pushboolean(L, 1);
        return 1;
    }
    return pushValue(L, ret, result);
}

int l_class_call_static(lua_State* L) {
    auto* c = checkClass(L, 1);
    const char* name = luaL_checkstring(L, 2);
    const int argCount = lua_gettop(L) - 2;
    auto* m = c->klass ? Il2cpp::GetClassMethod(c->klass, name, argCount) : nullptr;
    if (!m) return luaL_error(L, "AZ: static method not found: %s/%d", name, argCount);
    return invokeMethod(L, m, nullptr, 3);
}

int l_instance_call(lua_State* L) {
    auto* u = checkInstance(L, 1);
    auto* obj = getInstanceObject(u);
    if (!obj) return luaL_error(L, "AZ: selected object is no longer alive");
    const char* name = luaL_checkstring(L, 2);
    const int argCount = lua_gettop(L) - 2;
    auto* klass = Il2cpp::GetObjectClass(obj);
    auto* m = klass ? Il2cpp::GetClassMethod(klass, name, argCount) : nullptr;
    if (!m) return luaL_error(L, "AZ: instance method not found: %s/%d", name, argCount);
    return invokeMethod(L, m, obj, 3);
}

int l_class_new(lua_State* L) {
    auto* c = checkClass(L, 1);
    if (!c->klass) {
        lua_pushnil(L);
        return 1;
    }

    if (lua_gettop(L) != 1) {
        return luaL_error(L, "AZ: class:new() v0.1 currently supports parameterless constructors only");
    }

    Il2CppObject* obj = Il2cpp::NewObject(c->klass);
    if (!obj) {
        lua_pushnil(L);
        return 1;
    }

    auto* ctor = Il2cpp::GetClassMethod(c->klass, ".ctor", 0);
    if (ctor) {
        Il2CppException* exc = nullptr;
        Il2cpp::RuntimeInvoke(ctor, obj, nullptr, &exc);
        if (exc) {
            return luaL_error(L, "AZ: constructor threw a managed exception");
        }
    }
    return pushInstance(L, obj);
}

int l_call_exact(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    lua_getfield(L, 1, "declaring");
    const char* declaring = luaL_checkstring(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "name");
    const char* methodName = luaL_checkstring(L, -1);
    lua_pop(L, 1);

    std::vector<std::string> params;
    lua_getfield(L, 1, "params");
    if (lua_istable(L, -1)) {
        const lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, -1));
        for (lua_Integer i = 1; i <= n; ++i) {
            lua_rawgeti(L, -1, i);
            params.emplace_back(luaL_checkstring(L, -1));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    Il2CppClass* klass = Il2cpp::FindClass(declaring);
    if (!klass) return luaL_error(L, "AZ: declaring class not found: %s", declaring);

    MethodInfo* method = klass->getMethod(methodName, params);
    if (!method) return luaL_error(L, "AZ: exact method not found: %s::%s", declaring, methodName);

    Il2CppObject* instance = nullptr;
    if (!lua_isnil(L, 2)) {
        auto* u = static_cast<LuaInstance*>(luaL_testudata(L, 2, MT_INSTANCE));
        if (!u) return luaL_error(L, "AZ: Call.exact arg2 must be instance or nil");
        instance = getInstanceObject(u);
        if (!instance) return luaL_error(L, "AZ: selected object is no longer alive");
    }

    return invokeMethod(L, method, instance, 3);
}

int l_call_default(lua_State* L) {
    lua_newtable(L);
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "__az_default");
    if (!lua_isnoneornil(L, 1)) {
        lua_pushvalue(L, 1);
        lua_setfield(L, -2, "type");
    }
    return 1;
}

int l_call_pointer(lua_State* L) {
    const lua_Integer addr = luaL_checkinteger(L, 1);
    lua_newtable(L);
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "__az_pointer");
    lua_pushinteger(L, addr);
    lua_setfield(L, -2, "address");
    return 1;
}

int l_get_version(lua_State* L) {
    lua_pushliteral(L, "AZ Tool Core 0.1 / Lua 5.4.7");
    return 1;
}

void registerClass(lua_State* L) {
    luaL_newmetatable(L, MT_CLASS);
    lua_pushcfunction(L, l_class_tostring);
    lua_setfield(L, -2, "__tostring");

    lua_newtable(L);
    lua_pushcfunction(L, l_class_find_objects);
    lua_setfield(L, -2, "findObjects");
    lua_pushcfunction(L, l_class_find_objects);
    lua_setfield(L, -2, "findObjectsFresh");
    lua_pushcfunction(L, l_class_fields);
    lua_setfield(L, -2, "fields");
    lua_pushcfunction(L, l_class_get_static);
    lua_setfield(L, -2, "getStaticObject");
    lua_pushcfunction(L, l_class_set_field);
    lua_setfield(L, -2, "setField");
    lua_pushcfunction(L, l_class_new);
    lua_setfield(L, -2, "new");
    lua_pushcfunction(L, l_class_call_static);
    lua_setfield(L, -2, "callStatic");
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, l_class_from_name);
    lua_setfield(L, -2, "fromName");

    lua_newtable(L);
    lua_pushcfunction(L, l_class_from_name);
    lua_setfield(L, -2, "__call");
    lua_setmetatable(L, -2);

    lua_setglobal(L, "Class");
}

void registerInstance(lua_State* L) {
    luaL_newmetatable(L, MT_INSTANCE);
    lua_pushcfunction(L, l_instance_gc);
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, l_instance_tostring);
    lua_setfield(L, -2, "__tostring");

    lua_newtable(L);
    lua_pushcfunction(L, l_instance_get_address);
    lua_setfield(L, -2, "getAddress");
    lua_pushcfunction(L, l_instance_get_field);
    lua_setfield(L, -2, "getField");
    lua_pushcfunction(L, l_instance_get_field);
    lua_setfield(L, -2, "getFieldObject");
    lua_pushcfunction(L, l_instance_set_field);
    lua_setfield(L, -2, "setField");
    lua_pushcfunction(L, l_instance_call);
    lua_setfield(L, -2, "call");
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
}

void registerCall(lua_State* L) {
    lua_newtable(L);
    lua_pushcfunction(L, l_call_exact);
    lua_setfield(L, -2, "exact");
    lua_pushcfunction(L, l_call_default);
    lua_setfield(L, -2, "default");
    lua_pushcfunction(L, l_call_pointer);
    lua_setfield(L, -2, "pointer");
    lua_setglobal(L, "Call");
}

void registerAZ(lua_State* L) {
    lua_newtable(L);
    lua_pushcfunction(L, l_get_version);
    lua_setfield(L, -2, "version");
    lua_setglobal(L, "AZ");
}

} // namespace

namespace AZLua {

const char* Version() {
    return "AZ Tool Core 0.1 / Lua 5.4.7";
}

bool Init() {
    std::lock_guard<std::mutex> lock(g_luaMutex);
    if (g_L) return true;

    g_L = luaL_newstate();
    if (!g_L) return false;
    luaL_openlibs(g_L);

    lua_pushcfunction(g_L, l_print);
    lua_setglobal(g_L, "print");

    registerClass(g_L);
    registerInstance(g_L);
    registerCall(g_L);
    registerAZ(g_L);

    appendOutput("AZ Lua 5.4.7 initialized");
    appendOutput("Bindings: Class.fromName/findObjectsFresh/fields/getStaticObject/setField/new/callStatic");
    appendOutput("Bindings: Instance getAddress/getField/getFieldObject/setField/call");
    appendOutput("Bindings: Call.exact/default/pointer");
    return true;
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(g_luaMutex);
    if (g_L) {
        lua_close(g_L);
        g_L = nullptr;
    }
}

bool Run(const char* code) {
    if (!code) return false;
    if (!Init()) return false;

    std::lock_guard<std::mutex> lock(g_luaMutex);
    lua_settop(g_L, 0);

    if (luaL_loadbuffer(g_L, code, std::strlen(code), "AZScript") != LUA_OK) {
        appendOutput(std::string("[Lua load error] ") + lua_tostring(g_L, -1));
        lua_pop(g_L, 1);
        return false;
    }

    if (lua_pcall(g_L, 0, LUA_MULTRET, 0) != LUA_OK) {
        appendOutput(std::string("[Lua runtime error] ") + lua_tostring(g_L, -1));
        lua_pop(g_L, 1);
        return false;
    }

    appendOutput("[Lua] done");
    return true;
}

void Draw() {
    if (!g_L) Init();

    ImGui::TextUnformatted("AZ Script | Lua 5.4.7");
    ImGui::TextDisabled("Core-first build. Injection is intentionally outside this SO.");
    ImGui::Separator();

    ImVec2 avail = ImGui::GetContentRegionAvail();
    float editorHeight = std::max(180.0f, avail.y * 0.52f);
    ImGui::InputTextMultiline("##AZLuaEditor", g_editor, sizeof(g_editor),
                              ImVec2(-1.0f, editorHeight),
                              ImGuiInputTextFlags_AllowTabInput);

    if (ImGui::Button("Run")) {
        Run(g_editor);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear Output")) {
        std::lock_guard<std::mutex> lock(g_outputMutex);
        g_output.clear();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", Version());

    ImGui::Separator();
    ImGui::BeginChild("##AZLuaOutput", ImVec2(0, 0), true);
    {
        std::lock_guard<std::mutex> lock(g_outputMutex);
        for (const auto& line : g_output) {
            ImGui::TextUnformatted(line.c_str());
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 8.0f) {
            ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::EndChild();
}

} // namespace AZLua
