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
#include "Tool/Patcher.h"
#include "Tool/Tool.h"
#include "KittyMemory/KittyMemory.h"
#include "imgui/imgui.h"
#include <sys/mman.h>

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

// Forward declarations used by the call argument converter.
Il2CppObject* boxDefaultForType(Il2CppType* type);
bool tableHasBoolField(lua_State* L, int idx, const char* key);
lua_Integer tableIntegerField(lua_State* L, int idx, const char* key, lua_Integer fallback);

std::string g_selectedScript;
bool g_alertPending = false;
std::string g_alertText;

struct ReturnPatchRecord {
    void* target{};
    std::vector<uint8_t> original;
    uint32_t rootedHandle{};
};

std::mutex g_patchMutex;
std::unordered_map<uintptr_t, ReturnPatchRecord> g_returnPatches;


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

    // V7-style explicit argument markers. They avoid guessing whether integer 0
    // means "default value" or a real numeric zero, and make pointer arguments
    // opt-in rather than silently interpreting every Lua integer as an address.
    if (lua_istable(L, idx) && tableHasBoolField(L, idx, "__az_default")) {
        return boxDefaultForType(type);
    }
    if (lua_istable(L, idx) && tableHasBoolField(L, idx, "__az_pointer")) {
        const uintptr_t p = static_cast<uintptr_t>(tableIntegerField(L, idx, "address", 0));
        switch (type->type) {
            case IL2CPP_TYPE_I:
            case IL2CPP_TYPE_U:
            case IL2CPP_TYPE_PTR: {
                auto* klass = type->getClass();
                if (!klass) return nullptr;
                uintptr_t raw = p;
                return Il2cpp::GetBoxedValue(klass, &raw);
            }
            default:
                return nullptr;
        }
    }

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


// ===== AZ Core v0.2 extensions =====

Il2CppClass* resolveClassArg(lua_State* L, int idx) {
    if (auto* c = static_cast<LuaClass*>(luaL_testudata(L, idx, MT_CLASS))) {
        return c->klass;
    }
    if (lua_type(L, idx) == LUA_TSTRING) {
        return Il2cpp::FindClass(lua_tostring(L, idx));
    }
    return nullptr;
}

Il2CppType* effectiveValueType(Il2CppClass* klass) {
    if (!klass) return nullptr;
    if (Il2cpp::GetClassIsEnum(klass)) {
        auto* base = Il2cpp::GetEnumBaseType(klass);
        if (base) return base;
    }
    return Il2cpp::GetClassType(klass);
}

bool writeLuaPrimitiveToRaw(lua_State* L, int idx, Il2CppType* type, void* out, size_t capacity) {
    if (!type || !out) return false;

    auto need = [&](size_t n) { return capacity >= n; };
    switch (type->type) {
        case IL2CPP_TYPE_BOOLEAN:
            if (!need(1)) return false;
            *static_cast<uint8_t*>(out) = lua_toboolean(L, idx) ? 1 : 0;
            return true;
        case IL2CPP_TYPE_I1:
            if (!need(1)) return false;
            *static_cast<int8_t*>(out) = static_cast<int8_t>(luaL_checkinteger(L, idx));
            return true;
        case IL2CPP_TYPE_U1:
            if (!need(1)) return false;
            *static_cast<uint8_t*>(out) = static_cast<uint8_t>(luaL_checkinteger(L, idx));
            return true;
        case IL2CPP_TYPE_I2:
            if (!need(2)) return false;
            *static_cast<int16_t*>(out) = static_cast<int16_t>(luaL_checkinteger(L, idx));
            return true;
        case IL2CPP_TYPE_U2:
        case IL2CPP_TYPE_CHAR:
            if (!need(2)) return false;
            *static_cast<uint16_t*>(out) = static_cast<uint16_t>(luaL_checkinteger(L, idx));
            return true;
        case IL2CPP_TYPE_I4:
            if (!need(4)) return false;
            *static_cast<int32_t*>(out) = static_cast<int32_t>(luaL_checkinteger(L, idx));
            return true;
        case IL2CPP_TYPE_U4:
            if (!need(4)) return false;
            *static_cast<uint32_t*>(out) = static_cast<uint32_t>(luaL_checkinteger(L, idx));
            return true;
        case IL2CPP_TYPE_I8:
        case IL2CPP_TYPE_I:
            if (!need(sizeof(int64_t))) return false;
            *static_cast<int64_t*>(out) = static_cast<int64_t>(luaL_checkinteger(L, idx));
            return true;
        case IL2CPP_TYPE_U8:
        case IL2CPP_TYPE_U:
            if (!need(sizeof(uint64_t))) return false;
            *static_cast<uint64_t*>(out) = static_cast<uint64_t>(luaL_checkinteger(L, idx));
            return true;
        case IL2CPP_TYPE_R4:
            if (!need(sizeof(float))) return false;
            *static_cast<float*>(out) = static_cast<float>(luaL_checknumber(L, idx));
            return true;
        case IL2CPP_TYPE_R8:
            if (!need(sizeof(double))) return false;
            *static_cast<double*>(out) = static_cast<double>(luaL_checknumber(L, idx));
            return true;
        default:
            return false;
    }
}

Il2CppObject* boxDefaultForType(Il2CppType* type) {
    if (!type) return nullptr;
    auto* klass = type->getClass();
    if (!klass || !Il2cpp::GetClassIsValueType(klass)) return nullptr;

    const int32_t size = Il2cpp::GetClassValueSize(klass);
    if (size <= 0 || size > 4096) return nullptr;
    std::vector<uint8_t> zero(static_cast<size_t>(size), 0);
    return Il2cpp::GetBoxedValue(klass, zero.data());
}

bool tableHasBoolField(lua_State* L, int idx, const char* key) {
    if (!lua_istable(L, idx)) return false;
    idx = lua_absindex(L, idx);
    lua_getfield(L, idx, key);
    bool v = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return v;
}

lua_Integer tableIntegerField(lua_State* L, int idx, const char* key, lua_Integer fallback = 0) {
    if (!lua_istable(L, idx)) return fallback;
    idx = lua_absindex(L, idx);
    lua_getfield(L, idx, key);
    lua_Integer v = lua_isinteger(L, -1) ? lua_tointeger(L, -1) : fallback;
    lua_pop(L, 1);
    return v;
}

int l_instance_allocate(lua_State* L) {
    auto* klass = resolveClassArg(L, 1);
    if (!klass) return luaL_error(L, "AZ: Instance.allocate class not found");
    auto* obj = Il2cpp::NewObject(klass);
    if (!obj) {
        lua_pushnil(L);
        return 1;
    }
    return pushInstance(L, obj);
}

int l_instance_box(lua_State* L) {
    auto* klass = resolveClassArg(L, 1);
    if (!klass) return luaL_error(L, "AZ: Instance.box class not found");
    if (!Il2cpp::GetClassIsValueType(klass)) {
        return luaL_error(L, "AZ: Instance.box target is not a value type");
    }

    const int32_t size = Il2cpp::GetClassValueSize(klass);
    if (size <= 0 || size > 4096) {
        return luaL_error(L, "AZ: invalid value size: %d", size);
    }

    std::vector<uint8_t> raw(static_cast<size_t>(size), 0);
    if (!lua_isnoneornil(L, 2)) {
        if (auto* inst = static_cast<LuaInstance*>(luaL_testudata(L, 2, MT_INSTANCE))) {
            auto* obj = getInstanceObject(inst);
            if (!obj) return luaL_error(L, "AZ: source boxed object is no longer alive");
            auto* sourceClass = Il2cpp::GetObjectClass(obj);
            if (sourceClass != klass) return luaL_error(L, "AZ: boxed source type mismatch");
            void* unboxed = Il2cpp::GetUnboxedValue(obj);
            if (!unboxed) return luaL_error(L, "AZ: failed to unbox source value");
            std::memcpy(raw.data(), unboxed, static_cast<size_t>(size));
        } else {
            auto* valueType = effectiveValueType(klass);
            if (!writeLuaPrimitiveToRaw(L, 2, valueType, raw.data(), raw.size())) {
                return luaL_error(L, "AZ: no safe primitive conversion for %s", className(klass).c_str());
            }
        }
    }

    auto* boxed = Il2cpp::GetBoxedValue(klass, raw.data());
    return pushInstance(L, boxed);
}

int l_instance_materialize(lua_State* L) {
    if (lua_isnoneornil(L, 2)) {
        return luaL_error(L, "AZ: Instance.materialize requires a value");
    }
    return l_instance_box(L);
}

int l_array_create(lua_State* L) {
    auto* elementClass = resolveClassArg(L, 1);
    if (!elementClass) return luaL_error(L, "AZ: Array.create element class not found");
    luaL_checktype(L, 2, LUA_TTABLE);

    const size_t supplied = static_cast<size_t>(lua_rawlen(L, 2));
    size_t length = supplied;
    if (!lua_isnoneornil(L, 3)) {
        lua_Integer requested = luaL_checkinteger(L, 3);
        if (requested < 0) return luaL_error(L, "AZ: Array.create length must be >= 0");
        length = static_cast<size_t>(requested);
        if (length < supplied) {
            return luaL_error(L, "AZ: Array.create length is smaller than values table");
        }
    }

    auto* array = Il2cpp::ArrayNew(elementClass, static_cast<il2cpp_array_size_t>(length));
    if (!array) return luaL_error(L, "AZ: il2cpp_array_new failed");

    auto* arrayClass = Il2cpp::GetObjectClass(reinterpret_cast<Il2CppObject*>(array));
    uint32_t headerSize = il2cpp_array_object_header_size ? il2cpp_array_object_header_size()
                                                          : static_cast<uint32_t>(sizeof(_Il2CppArray));
    int elementSize = (il2cpp_class_array_element_size && arrayClass)
        ? il2cpp_class_array_element_size(arrayClass)
        : 0;
    if (elementSize <= 0) {
        elementSize = Il2cpp::GetClassIsValueType(elementClass)
            ? Il2cpp::GetClassValueSize(elementClass)
            : static_cast<int>(sizeof(void*));
    }
    if (headerSize < sizeof(Il2CppObject) || elementSize <= 0 || elementSize > 4096) {
        return luaL_error(L, "AZ: array metadata is unsafe (header=%u element=%d)", headerSize, elementSize);
    }

    uint8_t* data = reinterpret_cast<uint8_t*>(array) + headerSize;
    const bool valueType = Il2cpp::GetClassIsValueType(elementClass);
    auto* elementType = effectiveValueType(elementClass);

    for (size_t i = 0; i < supplied; ++i) {
        lua_rawgeti(L, 2, static_cast<lua_Integer>(i + 1));
        void* slot = data + (i * static_cast<size_t>(elementSize));

        if (valueType) {
            std::memset(slot, 0, static_cast<size_t>(elementSize));
            if (auto* inst = static_cast<LuaInstance*>(luaL_testudata(L, -1, MT_INSTANCE))) {
                auto* obj = getInstanceObject(inst);
                if (!obj || Il2cpp::GetObjectClass(obj) != elementClass) {
                    lua_pop(L, 1);
                    return luaL_error(L, "AZ: Array.create boxed value type mismatch at index %zu", i + 1);
                }
                void* raw = Il2cpp::GetUnboxedValue(obj);
                if (!raw) {
                    lua_pop(L, 1);
                    return luaL_error(L, "AZ: Array.create failed to unbox index %zu", i + 1);
                }
                const size_t valueSize = static_cast<size_t>(std::max(0, Il2cpp::GetClassValueSize(elementClass)));
                std::memcpy(slot, raw, std::min(valueSize, static_cast<size_t>(elementSize)));
            } else if (!writeLuaPrimitiveToRaw(L, -1, elementType, slot, static_cast<size_t>(elementSize))) {
                lua_pop(L, 1);
                return luaL_error(L, "AZ: unsupported array value at index %zu for %s",
                                  i + 1, className(elementClass).c_str());
            }
        } else {
            Il2CppObject* value = nullptr;
            auto* type = Il2cpp::GetClassType(elementClass);
            if (type && type->type == IL2CPP_TYPE_STRING) {
                if (!lua_isnil(L, -1)) {
                    value = reinterpret_cast<Il2CppObject*>(Il2cpp::NewString(luaL_checkstring(L, -1)));
                }
            } else if (!lua_isnil(L, -1)) {
                auto* inst = static_cast<LuaInstance*>(luaL_testudata(L, -1, MT_INSTANCE));
                if (!inst) {
                    lua_pop(L, 1);
                    return luaL_error(L, "AZ: reference array expects managed objects at index %zu", i + 1);
                }
                value = getInstanceObject(inst);
                if (!value) {
                    lua_pop(L, 1);
                    return luaL_error(L, "AZ: array object is no longer alive at index %zu", i + 1);
                }
            }

            auto** target = reinterpret_cast<void**>(slot);
            *target = value;
            if (il2cpp_gc_wbarrier_set_field) {
                il2cpp_gc_wbarrier_set_field(reinterpret_cast<Il2CppObject*>(array), target, value);
            }
        }
        lua_pop(L, 1);
    }

    return pushInstance(L, reinterpret_cast<Il2CppObject*>(array));
}

std::string safeScriptName(std::string name) {
    if (name.empty()) return {};
    if (name.find("..") != std::string::npos ||
        name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos) {
        return {};
    }
    if (name.size() < 4 || name.substr(name.size() - 4) != ".lua") name += ".lua";
    return name;
}

std::filesystem::path scriptDirectory() {
    std::string base = Il2cpp::getDataPath();
    if (base.empty() || base.rfind("unknown_", 0) == 0) return {};
    std::filesystem::path dir = std::filesystem::path(base) / "AZTool" / "scripts";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return {};
    return dir;
}

std::vector<std::string> listScripts() {
    std::vector<std::string> out;
    auto dir = scriptDirectory();
    if (dir.empty()) return out;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() == ".lua") out.push_back(entry.path().filename().string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool saveScript(const std::string& name, const char* content) {
    const std::string safe = safeScriptName(name);
    auto dir = scriptDirectory();
    if (safe.empty() || dir.empty()) return false;
    std::ofstream out(dir / safe, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(content, static_cast<std::streamsize>(std::strlen(content)));
    return out.good();
}

bool loadScript(const std::string& name) {
    const std::string safe = safeScriptName(name);
    auto dir = scriptDirectory();
    if (safe.empty() || dir.empty()) return false;
    std::ifstream in(dir / safe, std::ios::binary);
    if (!in) return false;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const size_t n = std::min(text.size(), sizeof(g_editor) - 1);
    std::memcpy(g_editor, text.data(), n);
    g_editor[n] = '\0';
    g_selectedScript = safe;
    std::snprintf(g_scriptName, sizeof(g_scriptName), "%s", safe.c_str());
    return true;
}

bool deleteScript(const std::string& name) {
    const std::string safe = safeScriptName(name);
    auto dir = scriptDirectory();
    if (safe.empty() || dir.empty()) return false;
    std::error_code ec;
    const bool removed = std::filesystem::remove(dir / safe, ec);
    return removed && !ec;
}

void releaseWidgetRef(lua_State* L, RetainedWidget& w) {
    if (L && w.callbackRef != LUA_NOREF && w.callbackRef != LUA_REFNIL) {
        luaL_unref(L, LUA_REGISTRYINDEX, w.callbackRef);
    }
    w.callbackRef = LUA_NOREF;
}

void releaseWindowRefs(lua_State* L, RetainedWindow& w) {
    for (auto& widget : w.widgets) releaseWidgetRef(L, widget);
}

RetainedWindow& getOrCreateWindowLocked(const std::string& id) {
    auto it = g_windows.find(id);
    if (it == g_windows.end()) {
        RetainedWindow w;
        w.id = id;
        w.title = id;
        it = g_windows.emplace(id, std::move(w)).first;
    }
    return it->second;
}

int captureCallback(lua_State* L, int idx) {
    if (lua_isnoneornil(L, idx)) return LUA_NOREF;
    luaL_checktype(L, idx, LUA_TFUNCTION);
    lua_pushvalue(L, idx);
    return luaL_ref(L, LUA_REGISTRYINDEX);
}

int l_ui_create_window(lua_State* L) {
    const char* id = luaL_checkstring(L, 1);
    const char* title = luaL_optstring(L, 2, id);
    std::lock_guard<std::mutex> lock(g_uiMutex);
    auto& w = getOrCreateWindowLocked(id);
    releaseWindowRefs(L, w);
    w.widgets.clear();
    w.title = title ? title : id;
    w.open = true;
    lua_pushboolean(L, 1);
    return 1;
}

int l_ui_remove_window(lua_State* L) {
    const char* id = luaL_checkstring(L, 1);
    std::lock_guard<std::mutex> lock(g_uiMutex);
    auto it = g_windows.find(id);
    if (it != g_windows.end()) {
        releaseWindowRefs(L, it->second);
        g_windows.erase(it);
    }
    return 0;
}

int l_ui_clear_window(lua_State* L) {
    const char* id = luaL_checkstring(L, 1);
    std::lock_guard<std::mutex> lock(g_uiMutex);
    auto& w = getOrCreateWindowLocked(id);
    releaseWindowRefs(L, w);
    w.widgets.clear();
    return 0;
}

int l_ui_set_window_open(lua_State* L) {
    const char* id = luaL_checkstring(L, 1);
    const bool open = lua_toboolean(L, 2);
    std::lock_guard<std::mutex> lock(g_uiMutex);
    getOrCreateWindowLocked(id).open = open;
    return 0;
}

RetainedWidget& addWidget(lua_State* L, const std::string& windowId, RetainedWidget widget) {
    std::lock_guard<std::mutex> lock(g_uiMutex);
    auto& w = getOrCreateWindowLocked(windowId);
    if (!widget.id.empty()) {
        for (auto& existing : w.widgets) {
            if (existing.id == widget.id) {
                releaseWidgetRef(L, existing);
                existing = std::move(widget);
                return existing;
            }
        }
    }
    w.widgets.push_back(std::move(widget));
    return w.widgets.back();
}

int l_ui_text(lua_State* L) {
    RetainedWidget w;
    w.kind = RetainedKind::Text;
    w.text = luaL_checkstring(L, 2);
    addWidget(L, luaL_checkstring(L, 1), std::move(w));
    return 0;
}

int l_ui_separator(lua_State* L) {
    RetainedWidget w;
    w.kind = RetainedKind::Separator;
    addWidget(L, luaL_checkstring(L, 1), std::move(w));
    return 0;
}

int l_ui_same_line(lua_State* L) {
    RetainedWidget w;
    w.kind = RetainedKind::SameLine;
    addWidget(L, luaL_checkstring(L, 1), std::move(w));
    return 0;
}

int l_ui_button(lua_State* L) {
    RetainedWidget w;
    w.kind = RetainedKind::Button;
    w.id = luaL_checkstring(L, 2);
    w.label = luaL_checkstring(L, 3);
    w.callbackRef = captureCallback(L, 4);
    addWidget(L, luaL_checkstring(L, 1), std::move(w));
    return 0;
}

int l_ui_checkbox(lua_State* L) {
    RetainedWidget w;
    w.kind = RetainedKind::Checkbox;
    w.id = luaL_checkstring(L, 2);
    w.label = luaL_checkstring(L, 3);
    w.boolValue = lua_toboolean(L, 4);
    w.callbackRef = captureCallback(L, 5);
    addWidget(L, luaL_checkstring(L, 1), std::move(w));
    return 0;
}

int l_ui_input_text(lua_State* L) {
    RetainedWidget w;
    w.kind = RetainedKind::InputText;
    w.id = luaL_checkstring(L, 2);
    w.label = luaL_checkstring(L, 3);
    w.text = luaL_optstring(L, 4, "");
    w.callbackRef = captureCallback(L, 5);
    addWidget(L, luaL_checkstring(L, 1), std::move(w));
    return 0;
}

int l_ui_input_int(lua_State* L) {
    RetainedWidget w;
    w.kind = RetainedKind::InputInt;
    w.id = luaL_checkstring(L, 2);
    w.label = luaL_checkstring(L, 3);
    w.intValue = static_cast<int>(luaL_optinteger(L, 4, 0));
    w.callbackRef = captureCallback(L, 5);
    addWidget(L, luaL_checkstring(L, 1), std::move(w));
    return 0;
}

int l_ui_input_float(lua_State* L) {
    RetainedWidget w;
    w.kind = RetainedKind::InputFloat;
    w.id = luaL_checkstring(L, 2);
    w.label = luaL_checkstring(L, 3);
    w.floatValue = static_cast<float>(luaL_optnumber(L, 4, 0.0));
    w.callbackRef = captureCallback(L, 5);
    addWidget(L, luaL_checkstring(L, 1), std::move(w));
    return 0;
}

int l_ui_slider_int(lua_State* L) {
    RetainedWidget w;
    w.kind = RetainedKind::SliderInt;
    w.id = luaL_checkstring(L, 2);
    w.label = luaL_checkstring(L, 3);
    w.intValue = static_cast<int>(luaL_checkinteger(L, 4));
    w.minValue = static_cast<int>(luaL_checkinteger(L, 5));
    w.maxValue = static_cast<int>(luaL_checkinteger(L, 6));
    w.callbackRef = captureCallback(L, 7);
    addWidget(L, luaL_checkstring(L, 1), std::move(w));
    return 0;
}

int l_ui_combo(lua_State* L) {
    RetainedWidget w;
    w.kind = RetainedKind::Combo;
    w.id = luaL_checkstring(L, 2);
    w.label = luaL_checkstring(L, 3);
    luaL_checktype(L, 4, LUA_TTABLE);
    const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(L, 4));
    for (lua_Integer i = 1; i <= count; ++i) {
        lua_rawgeti(L, 4, i);
        w.options.emplace_back(luaL_checkstring(L, -1));
        lua_pop(L, 1);
    }
    w.intValue = static_cast<int>(luaL_optinteger(L, 5, 1));
    w.callbackRef = captureCallback(L, 6);
    addWidget(L, luaL_checkstring(L, 1), std::move(w));
    return 0;
}

RetainedWidget* findWidgetLocked(const std::string& wid, const std::string& id) {
    auto wit = g_windows.find(wid);
    if (wit == g_windows.end()) return nullptr;
    for (auto& w : wit->second.widgets) if (w.id == id) return &w;
    return nullptr;
}

int l_ui_get_value(lua_State* L) {
    const char* windowId = luaL_checkstring(L, 1);
    const char* widgetId = luaL_checkstring(L, 2);
    std::lock_guard<std::mutex> lock(g_uiMutex);
    auto* w = findWidgetLocked(windowId, widgetId);
    if (!w) {
        lua_pushnil(L);
        return 1;
    }
    switch (w->kind) {
        case RetainedKind::Checkbox:
            lua_pushboolean(L, w->boolValue);
            break;
        case RetainedKind::InputText:
            lua_pushlstring(L, w->text.c_str(), w->text.size());
            break;
        case RetainedKind::InputFloat:
            lua_pushnumber(L, w->floatValue);
            break;
        case RetainedKind::InputInt:
        case RetainedKind::SliderInt:
        case RetainedKind::Combo:
            lua_pushinteger(L, w->intValue);
            break;
        default:
            lua_pushnil(L);
            break;
    }
    return 1;
}

int l_ui_set_value(lua_State* L) {
    const char* windowId = luaL_checkstring(L, 1);
    const char* widgetId = luaL_checkstring(L, 2);
    std::lock_guard<std::mutex> lock(g_uiMutex);
    auto* w = findWidgetLocked(windowId, widgetId);
    if (!w) return luaL_error(L, "AZ: retained widget not found");
    switch (w->kind) {
        case RetainedKind::Checkbox:
            w->boolValue = lua_toboolean(L, 3);
            break;
        case RetainedKind::InputText:
            w->text = luaL_checkstring(L, 3);
            break;
        case RetainedKind::InputFloat:
            w->floatValue = static_cast<float>(luaL_checknumber(L, 3));
            break;
        case RetainedKind::InputInt:
        case RetainedKind::SliderInt:
        case RetainedKind::Combo:
            w->intValue = static_cast<int>(luaL_checkinteger(L, 3));
            break;
        default:
            return luaL_error(L, "AZ: widget has no retained value");
    }
    return 0;
}

int l_gg_toast(lua_State* L) {
    appendOutput(std::string("[toast] ") + luaL_checkstring(L, 1));
    return 0;
}

int l_gg_alert(lua_State* L) {
    std::lock_guard<std::mutex> lock(g_uiMutex);
    g_alertText = luaL_checkstring(L, 1);
    g_alertPending = true;
    lua_pushinteger(L, 1);
    return 1;
}

extern bool collapsed;

int l_gg_set_visible(lua_State* L) {
    collapsed = !lua_toboolean(L, 1);
    return 0;
}

int l_gg_is_visible(lua_State* L) {
    lua_pushboolean(L, !collapsed);
    return 1;
}

int l_gg_sleep(lua_State* L) {
    lua_Integer ms = luaL_checkinteger(L, 1);
    if (ms < 0) ms = 0;
    usleep(static_cast<useconds_t>(ms * 1000));
    return 0;
}

int l_gg_get_target_package(lua_State* L) {
    std::string p = Il2cpp::getPackageName();
    lua_pushlstring(L, p.c_str(), p.size());
    return 1;
}

int l_gg_get_target_info(lua_State* L) {
    lua_newtable(L);
    std::string p = Il2cpp::getPackageName();
    std::string v = Il2cpp::getGameVersion();
    std::string u = Il2cpp::getUnityVersion();
    lua_pushlstring(L, p.c_str(), p.size());
    lua_setfield(L, -2, "packageName");
    lua_pushlstring(L, v.c_str(), v.size());
    lua_setfield(L, -2, "versionName");
    lua_pushlstring(L, u.c_str(), u.size());
    lua_setfield(L, -2, "unityVersion");
    return 1;
}

void registerInstanceFactory(lua_State* L) {
    lua_newtable(L);
    lua_pushcfunction(L, l_instance_allocate);
    lua_setfield(L, -2, "allocate");
    lua_pushcfunction(L, l_instance_allocate);
    lua_setfield(L, -2, "Allocate");
    lua_pushcfunction(L, l_instance_box);
    lua_setfield(L, -2, "box");
    lua_pushcfunction(L, l_instance_box);
    lua_setfield(L, -2, "Box");
    lua_pushcfunction(L, l_instance_materialize);
    lua_setfield(L, -2, "materialize");
    lua_pushcfunction(L, l_instance_materialize);
    lua_setfield(L, -2, "Materialize");
    lua_setglobal(L, "Instance");
}

void registerArray(lua_State* L) {
    lua_newtable(L);
    lua_pushcfunction(L, l_array_create);
    lua_setfield(L, -2, "create");
    lua_pushcfunction(L, l_array_create);
    lua_setfield(L, -2, "Create");
    lua_setglobal(L, "Array");
}

void registerUI(lua_State* L) {
    lua_newtable(L);
    lua_pushcfunction(L, l_ui_create_window);
    lua_setfield(L, -2, "window");
    lua_pushcfunction(L, l_ui_create_window);
    lua_setfield(L, -2, "createWindow");
    lua_pushcfunction(L, l_ui_remove_window);
    lua_setfield(L, -2, "removeWindow");
    lua_pushcfunction(L, l_ui_clear_window);
    lua_setfield(L, -2, "clearWindow");
    lua_pushcfunction(L, l_ui_set_window_open);
    lua_setfield(L, -2, "setWindowOpen");
    lua_pushcfunction(L, l_ui_text);
    lua_setfield(L, -2, "text");
    lua_pushcfunction(L, l_ui_separator);
    lua_setfield(L, -2, "separator");
    lua_pushcfunction(L, l_ui_same_line);
    lua_setfield(L, -2, "sameLine");
    lua_pushcfunction(L, l_ui_button);
    lua_setfield(L, -2, "button");
    lua_pushcfunction(L, l_ui_checkbox);
    lua_setfield(L, -2, "checkbox");
    lua_pushcfunction(L, l_ui_input_text);
    lua_setfield(L, -2, "inputText");
    lua_pushcfunction(L, l_ui_input_int);
    lua_setfield(L, -2, "inputInt");
    lua_pushcfunction(L, l_ui_input_float);
    lua_setfield(L, -2, "inputFloat");
    lua_pushcfunction(L, l_ui_slider_int);
    lua_setfield(L, -2, "sliderInt");
    lua_pushcfunction(L, l_ui_combo);
    lua_setfield(L, -2, "combo");
    lua_pushcfunction(L, l_ui_get_value);
    lua_setfield(L, -2, "getValue");
    lua_pushcfunction(L, l_ui_set_value);
    lua_setfield(L, -2, "setValue");
    lua_setglobal(L, "UI");
}

void registerGG(lua_State* L) {
    lua_newtable(L);
    lua_pushcfunction(L, l_gg_alert);
    lua_setfield(L, -2, "alert");
    lua_pushcfunction(L, l_gg_toast);
    lua_setfield(L, -2, "toast");
    lua_pushcfunction(L, l_gg_sleep);
    lua_setfield(L, -2, "sleep");
    lua_pushcfunction(L, l_gg_set_visible);
    lua_setfield(L, -2, "setVisible");
    lua_pushcfunction(L, l_gg_is_visible);
    lua_setfield(L, -2, "isVisible");
    lua_pushcfunction(L, l_gg_get_target_package);
    lua_setfield(L, -2, "getTargetPackage");
    lua_pushcfunction(L, l_gg_get_target_info);
    lua_setfield(L, -2, "getTargetInfo");

    // Common GameGuardian numeric constants. Memory scanning itself is added in
    // a later AZ layer; keeping the constants stable makes scripts portable.
    lua_pushinteger(L, 1); lua_setfield(L, -2, "TYPE_BYTE");
    lua_pushinteger(L, 2); lua_setfield(L, -2, "TYPE_WORD");
    lua_pushinteger(L, 4); lua_setfield(L, -2, "TYPE_DWORD");
    lua_pushinteger(L, 16); lua_setfield(L, -2, "TYPE_FLOAT");
    lua_pushinteger(L, 64); lua_setfield(L, -2, "TYPE_DOUBLE");
    lua_pushinteger(L, 32); lua_setfield(L, -2, "TYPE_QWORD");
    lua_setglobal(L, "gg");
}

bool invokeRetainedCallback(int ref, int kind, bool bv, int iv, float fv, const std::string& sv) {
    if (!g_L || ref == LUA_NOREF || ref == LUA_REFNIL) return false;
    std::lock_guard<std::mutex> lock(g_luaMutex);
    lua_rawgeti(g_L, LUA_REGISTRYINDEX, ref);
    if (!lua_isfunction(g_L, -1)) {
        lua_pop(g_L, 1);
        return false;
    }

    int argc = 0;
    switch (kind) {
        case 1: lua_pushboolean(g_L, bv); argc = 1; break;
        case 2: lua_pushinteger(g_L, iv); argc = 1; break;
        case 3: lua_pushnumber(g_L, fv); argc = 1; break;
        case 4: lua_pushlstring(g_L, sv.c_str(), sv.size()); argc = 1; break;
        default: break;
    }

    if (lua_pcall(g_L, argc, 0, 0) != LUA_OK) {
        appendOutput(std::string("[UI callback error] ") + (lua_tostring(g_L, -1) ? lua_tostring(g_L, -1) : "unknown"));
        lua_pop(g_L, 1);
        return false;
    }
    return true;
}

int l_get_version(lua_State* L) {
    lua_pushliteral(L, "AZ Tool Core 0.2 / Lua 5.4.7");
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
    return "AZ Tool Core 0.2 / Lua 5.4.7";
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
    registerInstanceFactory(g_L);
    registerArray(g_L);
    registerCall(g_L);
    registerUI(g_L);
    registerGG(g_L);
    registerAZ(g_L);

    appendOutput("AZ Lua 5.4.7 initialized");
    appendOutput("Bindings: Class.fromName/findObjectsFresh/fields/getStaticObject/setField/new/callStatic");
    appendOutput("Bindings: Instance getAddress/getField/getFieldObject/setField/call");
    appendOutput("Bindings: Call.exact/default/pointer");
    appendOutput("Bindings: Instance.allocate/box/materialize + Array.create");
    appendOutput("Bindings: retained UI.* + gg alert/toast/visibility/sleep/target-info");
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

    static bool triedLastScript = false;
    if (!triedLastScript) {
        triedLastScript = true;
        loadScript("_last.lua");
    }

    ImGui::TextUnformatted("AZ Script | Lua 5.4.7");
    ImGui::TextDisabled("Standalone core. Script storage uses the game's persistentDataPath/AZTool/scripts.");
    ImGui::Separator();

    ImGui::InputText("Name", g_scriptName, sizeof(g_scriptName));
    if (ImGui::Button("Save")) {
        if (saveScript(g_scriptName, g_editor)) {
            appendOutput(std::string("Saved: ") + safeScriptName(g_scriptName));
            g_selectedScript = safeScriptName(g_scriptName);
        } else {
            appendOutput("Save failed");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Load")) {
        if (!loadScript(g_scriptName)) appendOutput("Load failed");
        else appendOutput(std::string("Loaded: ") + safeScriptName(g_scriptName));
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete")) {
        if (deleteScript(g_scriptName)) appendOutput(std::string("Deleted: ") + safeScriptName(g_scriptName));
        else appendOutput("Delete failed");
    }

    const auto scripts = listScripts();
    if (!scripts.empty()) {
        const char* preview = g_selectedScript.empty() ? scripts.front().c_str() : g_selectedScript.c_str();
        if (ImGui::BeginCombo("Scripts", preview)) {
            for (const auto& name : scripts) {
                const bool selected = (name == g_selectedScript);
                if (ImGui::Selectable(name.c_str(), selected)) {
                    loadScript(name);
                    appendOutput(std::string("Loaded: ") + name);
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    ImGui::Separator();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float editorHeight = std::max(180.0f, avail.y * 0.50f);
    ImGui::InputTextMultiline("##AZLuaEditor", g_editor, sizeof(g_editor),
                              ImVec2(-1.0f, editorHeight),
                              ImGuiInputTextFlags_AllowTabInput);

    if (ImGui::Button("Run")) {
        saveScript("_last.lua", g_editor);
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

void DrawRetainedWindows() {
    if (!g_L) return;

    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lock(g_uiMutex);
        ids.reserve(g_windows.size());
        for (const auto& it : g_windows) ids.push_back(it.first);
    }

    for (const auto& id : ids) {
        struct Pending {
            int ref{LUA_NOREF};
            int kind{};
            bool bv{};
            int iv{};
            float fv{};
            std::string sv;
        } pending;

        {
            std::lock_guard<std::mutex> lock(g_uiMutex);
            auto it = g_windows.find(id);
            if (it == g_windows.end()) continue;
            auto& win = it->second;
            if (!win.open) continue;

            bool open = win.open;
            if (ImGui::Begin(win.title.c_str(), &open)) {
                for (size_t wi = 0; wi < win.widgets.size(); ++wi) {
                    auto& w = win.widgets[wi];
                    ImGui::PushID((id + ":" + w.id + ":" + std::to_string(wi)).c_str());

                    switch (w.kind) {
                        case RetainedKind::Text:
                            ImGui::TextWrapped("%s", w.text.c_str());
                            break;
                        case RetainedKind::Separator:
                            ImGui::Separator();
                            break;
                        case RetainedKind::SameLine:
                            ImGui::SameLine();
                            break;
                        case RetainedKind::Button:
                            if (ImGui::Button(w.label.c_str())) {
                                pending.ref = w.callbackRef;
                            }
                            break;
                        case RetainedKind::Checkbox: {
                            bool v = w.boolValue;
                            if (ImGui::Checkbox(w.label.c_str(), &v)) {
                                w.boolValue = v;
                                pending.ref = w.callbackRef;
                                pending.kind = 1;
                                pending.bv = v;
                            }
                            break;
                        }
                        case RetainedKind::InputText: {
                            char buf[1024]{};
                            std::snprintf(buf, sizeof(buf), "%s", w.text.c_str());
                            if (ImGui::InputText(w.label.c_str(), buf, sizeof(buf),
                                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
                                w.text = buf;
                                pending.ref = w.callbackRef;
                                pending.kind = 4;
                                pending.sv = w.text;
                            } else if (std::strcmp(buf, w.text.c_str()) != 0) {
                                w.text = buf;
                            }
                            break;
                        }
                        case RetainedKind::InputInt: {
                            int v = w.intValue;
                            if (ImGui::InputInt(w.label.c_str(), &v, 1, 100,
                                                ImGuiInputTextFlags_EnterReturnsTrue)) {
                                w.intValue = v;
                                pending.ref = w.callbackRef;
                                pending.kind = 2;
                                pending.iv = v;
                            } else {
                                w.intValue = v;
                            }
                            break;
                        }
                        case RetainedKind::InputFloat: {
                            float v = w.floatValue;
                            if (ImGui::InputFloat(w.label.c_str(), &v, 0.1f, 1.0f, "%.3f",
                                                  ImGuiInputTextFlags_EnterReturnsTrue)) {
                                w.floatValue = v;
                                pending.ref = w.callbackRef;
                                pending.kind = 3;
                                pending.fv = v;
                            } else {
                                w.floatValue = v;
                            }
                            break;
                        }
                        case RetainedKind::SliderInt: {
                            int v = w.intValue;
                            if (ImGui::SliderInt(w.label.c_str(), &v, w.minValue, w.maxValue)) {
                                w.intValue = v;
                                pending.ref = w.callbackRef;
                                pending.kind = 2;
                                pending.iv = v;
                            }
                            break;
                        }
                        case RetainedKind::Combo: {
                            int oneBased = std::max(1, w.intValue);
                            int zeroBased = oneBased - 1;
                            const char* preview = (zeroBased >= 0 && zeroBased < static_cast<int>(w.options.size()))
                                ? w.options[zeroBased].c_str() : "";
                            if (ImGui::BeginCombo(w.label.c_str(), preview)) {
                                for (int oi = 0; oi < static_cast<int>(w.options.size()); ++oi) {
                                    bool selected = oi == zeroBased;
                                    if (ImGui::Selectable(w.options[oi].c_str(), selected)) {
                                        w.intValue = oi + 1;
                                        pending.ref = w.callbackRef;
                                        pending.kind = 2;
                                        pending.iv = w.intValue;
                                    }
                                    if (selected) ImGui::SetItemDefaultFocus();
                                }
                                ImGui::EndCombo();
                            }
                            break;
                        }
                    }

                    ImGui::PopID();
                    if (pending.ref != LUA_NOREF) break;
                }
            }
            ImGui::End();
            win.open = open;
        }

        if (pending.ref != LUA_NOREF) {
            invokeRetainedCallback(pending.ref, pending.kind, pending.bv, pending.iv, pending.fv, pending.sv);
        }
    }

    bool alert = false;
    std::string alertText;
    {
        std::lock_guard<std::mutex> lock(g_uiMutex);
        if (g_alertPending) {
            g_alertPending = false;
            alert = true;
            alertText = g_alertText;
        }
    }
    if (alert) ImGui::OpenPopup("AZ Alert");
    if (ImGui::BeginPopupModal("AZ Alert", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        static std::string shown;
        if (alert) shown = alertText;
        ImGui::TextWrapped("%s", shown.c_str());
        if (ImGui::Button("OK", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

} // namespace AZLua
