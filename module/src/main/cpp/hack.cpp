#include "hack.h"
#include "il2cpp_dump.h"
#include "log.h"
#include "xdl.h"
#include "il2cpp-tabledefs.h"
#include "il2cpp-class.h"

#include <cstring>
#include <cstdio>
#include <unistd.h>
#include <sys/system_properties.h>
#include <dlfcn.h>
#include <jni.h>
#include <thread>
#include <sys/mman.h>
#include <linux/unistd.h>
#include <array>
#include <string>
#include <vector>
#include <set>
#include <fstream>
#include <algorithm>

#define DO_API(r, n, p) extern r (*n) p
#include "il2cpp-api-functions.h"
#undef DO_API

static std::string g_status_path;

static void status(const std::string &s) {
    LOGI("[INKVASION AUTO] %s", s.c_str());
    if (!g_status_path.empty()) {
        std::ofstream f(g_status_path, std::ios::app);
        if (f.good()) f << s << "\n";
    }
}

static bool invoke_checked(const MethodInfo *method, void *obj, void **params, Il2CppObject **out = nullptr) {
    if (!method || !il2cpp_runtime_invoke) return false;
    Il2CppException *exc = nullptr;
    Il2CppObject *r = il2cpp_runtime_invoke(method, obj, params, &exc);
    if (exc) {
        LOGE("[INKVASION AUTO] managed exception method=%s exc=%p",
             il2cpp_method_get_name ? il2cpp_method_get_name(method) : "?", exc);
        return false;
    }
    if (out) *out = r;
    return true;
}

static const MethodInfo *find_method(Il2CppClass *klass, const char *name, int argc) {
    for (Il2CppClass *k = klass; k; k = il2cpp_class_get_parent(k)) {
        void *iter = nullptr;
        while (const MethodInfo *m = il2cpp_class_get_methods(k, &iter)) {
            const char *mn = il2cpp_method_get_name(m);
            if (mn && std::strcmp(mn, name) == 0 &&
                (int)il2cpp_method_get_param_count(m) == argc) {
                return m;
            }
        }
    }
    return nullptr;
}

static FieldInfo *find_field(Il2CppClass *klass, const char *name) {
    for (Il2CppClass *k = klass; k; k = il2cpp_class_get_parent(k)) {
        if (auto *f = il2cpp_class_get_field_from_name(k, name)) return f;
    }
    return nullptr;
}

static PropertyInfo *find_property(Il2CppClass *klass, const char *name) {
    for (Il2CppClass *k = klass; k; k = il2cpp_class_get_parent(k)) {
        void *iter = nullptr;
        while (const PropertyInfo *pc = il2cpp_class_get_properties(k, &iter)) {
            auto *p = const_cast<PropertyInfo *>(pc);
            const char *pn = il2cpp_property_get_name(p);
            if (pn && std::strcmp(pn, name) == 0) return p;
        }
    }
    return nullptr;
}

static Il2CppClass *find_class_short(const char *short_name, const char *ns_hint = nullptr) {
    size_t ac = 0;
    auto domain = il2cpp_domain_get();
    auto assemblies = il2cpp_domain_get_assemblies(domain, &ac);
    for (size_t i = 0; i < ac; ++i) {
        auto image = il2cpp_assembly_get_image(assemblies[i]);
        if (!image || !il2cpp_image_get_class_count || !il2cpp_image_get_class) continue;
        size_t cc = il2cpp_image_get_class_count(image);
        for (size_t j = 0; j < cc; ++j) {
            auto *kc = const_cast<Il2CppClass *>(il2cpp_image_get_class(image, j));
            if (!kc) continue;
            const char *n = il2cpp_class_get_name(kc);
            if (!n || std::strcmp(n, short_name) != 0) continue;
            if (ns_hint && *ns_hint) {
                const char *ns = il2cpp_class_get_namespace(kc);
                if (!ns || std::strstr(ns, ns_hint) == nullptr) continue;
            }
            return kc;
        }
    }
    return nullptr;
}

static Il2CppClass *find_unity_class(const char *name) {
    size_t ac = 0;
    auto domain = il2cpp_domain_get();
    auto assemblies = il2cpp_domain_get_assemblies(domain, &ac);
    for (size_t i = 0; i < ac; ++i) {
        auto image = il2cpp_assembly_get_image(assemblies[i]);
        if (!image) continue;
        auto *k = il2cpp_class_from_name(image, "UnityEngine", name);
        if (k) return k;
    }
    return nullptr;
}

static const MethodInfo *find_type_search_method(Il2CppClass *klass, const char *name) {
    void *iter = nullptr;
    while (const MethodInfo *m = il2cpp_class_get_methods(klass, &iter)) {
        const char *mn = il2cpp_method_get_name(m);
        if (!mn || std::strcmp(mn, name) != 0 || il2cpp_method_get_param_count(m) != 1) continue;
        const Il2CppType *pt = il2cpp_method_get_param(m, 0);
        Il2CppClass *pc = il2cpp_class_from_type(pt);
        const char *pn = pc ? il2cpp_class_get_name(pc) : nullptr;
        if (pn && std::strcmp(pn, "Type") == 0) return m;
    }
    return nullptr;
}

static Il2CppObject *first_from_array(Il2CppObject *obj) {
    auto *arr = reinterpret_cast<Il2CppArray *>(obj);
    if (!arr || arr->max_length == 0) return nullptr;
    auto **items = reinterpret_cast<Il2CppObject **>(arr->vector);
    for (size_t i = 0; i < arr->max_length; ++i) {
        if (items[i]) return items[i];
    }
    return nullptr;
}

static Il2CppObject *find_live(Il2CppClass *target) {
    if (!target) return nullptr;
    Il2CppObject *type_obj = il2cpp_type_get_object(il2cpp_class_get_type(target));
    if (!type_obj) return nullptr;

    if (Il2CppClass *uo = find_unity_class("Object")) {
        if (const MethodInfo *m = find_type_search_method(uo, "FindObjectsOfType")) {
            void *p[1] = {type_obj};
            Il2CppObject *r = nullptr;
            if (invoke_checked(m, nullptr, p, &r) && r) {
                if (auto *x = first_from_array(r)) return x;
            }
        }
    }

    if (Il2CppClass *res = find_unity_class("Resources")) {
        if (const MethodInfo *m = find_type_search_method(res, "FindObjectsOfTypeAll")) {
            void *p[1] = {type_obj};
            Il2CppObject *r = nullptr;
            if (invoke_checked(m, nullptr, p, &r) && r) {
                if (auto *x = first_from_array(r)) return x;
            }
        }
    }
    return nullptr;
}

static bool call0(Il2CppObject *obj, const char *name, Il2CppObject **out) {
    if (!obj) return false;
    auto *m = find_method(obj->klass, name, 0);
    if (!m) return false;
    void *recv = (il2cpp_method_is_instance && !il2cpp_method_is_instance(m)) ? nullptr : obj;
    return invoke_checked(m, recv, nullptr, out);
}

static bool call0_class(Il2CppClass *klass, Il2CppObject *obj, const char *name, Il2CppObject **out) {
    auto *m = find_method(klass, name, 0);
    if (!m) return false;
    void *recv = (il2cpp_method_is_instance && !il2cpp_method_is_instance(m)) ? nullptr : obj;
    return invoke_checked(m, recv, nullptr, out);
}

static int boxed_i32(Il2CppObject *o, int fallback = -1) {
    if (!o || !il2cpp_object_unbox) return fallback;
    void *p = il2cpp_object_unbox(o);
    return p ? *reinterpret_cast<int32_t *>(p) : fallback;
}

static int collection_count(Il2CppObject *list) {
    if (!list) return -1;
    Il2CppObject *r = nullptr;
    if (!call0(list, "get_Count", &r) || !r) return -1;
    return boxed_i32(r, -1);
}

static Il2CppObject *collection_item(Il2CppObject *list, int index) {
    if (!list) return nullptr;
    auto *m = find_method(list->klass, "get_Item", 1);
    if (!m) return nullptr;
    int32_t idx = index;
    void *p[1] = {&idx};
    Il2CppObject *r = nullptr;
    if (!invoke_checked(m, list, p, &r)) return nullptr;
    return r;
}

static bool read_i32_member(Il2CppObject *obj, const char *name, int &out) {
    if (!obj) return false;
    if (FieldInfo *f = find_field(obj->klass, name)) {
        int32_t v = 0;
        il2cpp_field_get_value(obj, f, &v);
        out = v;
        return true;
    }

    if (PropertyInfo *p = find_property(obj->klass, name)) {
        const MethodInfo *gm = il2cpp_property_get_get_method(p);
        if (gm) {
            Il2CppObject *r = nullptr;
            if (invoke_checked(gm, obj, nullptr, &r) && r) {
                out = boxed_i32(r, 0);
                return true;
            }
        }
    }

    std::string getter = std::string("get_") + name;
    if (const MethodInfo *gm = find_method(obj->klass, getter.c_str(), 0)) {
        Il2CppObject *r = nullptr;
        if (invoke_checked(gm, obj, nullptr, &r) && r) {
            out = boxed_i32(r, 0);
            return true;
        }
    }
    return false;
}

static bool set_i32_property(Il2CppObject *obj, const char *name, int32_t value) {
    if (!obj) return false;
    std::string setter = std::string("set_") + name;
    const MethodInfo *m = find_method(obj->klass, setter.c_str(), 1);
    if (!m) return false;
    void *p[1] = {&value};
    return invoke_checked(m, obj, p, nullptr);
}

static bool list_add_i32(Il2CppObject *list, int32_t value) {
    if (!list) return false;
    const MethodInfo *m = find_method(list->klass, "Add", 1);
    if (!m) return false;
    void *p[1] = {&value};
    return invoke_checked(m, list, p, nullptr);
}

static bool list_add_obj(Il2CppObject *list, Il2CppObject *value) {
    if (!list || !value) return false;
    const MethodInfo *m = find_method(list->klass, "Add", 1);
    if (!m) return false;
    void *p[1] = {value};
    return invoke_checked(m, list, p, nullptr);
}

static Il2CppObject *new_object(Il2CppClass *klass) {
    if (!klass) return nullptr;
    auto *o = il2cpp_object_new(klass);
    if (!o) return nullptr;
    il2cpp_runtime_object_init(o);
    return o;
}

static Il2CppObject *new_same_list(Il2CppObject *seed_list) {
    return seed_list ? new_object(seed_list->klass) : nullptr;
}

static Il2CppObject *make_generic_list(Il2CppClass *element_class) {
    if (!element_class) return nullptr;
    const Il2CppImage *corlib = il2cpp_get_corlib();
    if (!corlib) return nullptr;

    Il2CppClass *list_def = il2cpp_class_from_name(corlib, "System.Collections.Generic", "List\`1");
    Il2CppClass *type_class = il2cpp_class_from_name(corlib, "System", "Type");
    if (!list_def || !type_class) return nullptr;

    Il2CppObject *list_type_obj = il2cpp_type_get_object(il2cpp_class_get_type(list_def));
    Il2CppObject *elem_type_obj = il2cpp_type_get_object(il2cpp_class_get_type(element_class));
    if (!list_type_obj || !elem_type_obj) return nullptr;

    Il2CppArray *type_args = il2cpp_array_new(type_class, 1);
    if (!type_args) return nullptr;
    reinterpret_cast<Il2CppObject **>(type_args->vector)[0] = elem_type_obj;

    const MethodInfo *make = find_method(type_class, "MakeGenericType", 1);
    if (!make) return nullptr;
    void *p[1] = {type_args};
    Il2CppObject *closed_type_obj = nullptr;
    if (!invoke_checked(make, list_type_obj, p, &closed_type_obj) || !closed_type_obj) return nullptr;

    Il2CppClass *closed = il2cpp_class_from_system_type(
            reinterpret_cast<Il2CppReflectionType *>(closed_type_obj));
    return new_object(closed);
}

static bool set_obj_property(Il2CppObject *obj, const char *name, Il2CppObject *value) {
    if (!obj) return false;
    std::string setter = std::string("set_") + name;
    const MethodInfo *m = find_method(obj->klass, setter.c_str(), 1);
    if (!m) return false;
    void *p[1] = {value};
    return invoke_checked(m, obj, p, nullptr);
}

static std::vector<int> collect_ids(Il2CppObject *list, const char *member) {
    std::set<int> uniq;
    int n = collection_count(list);
    if (n < 0) return {};
    for (int i = 0; i < n; ++i) {
        Il2CppObject *row = collection_item(list, i);
        int id = 0;
        if (row && read_i32_member(row, member, id)) uniq.insert(id);
    }
    return std::vector<int>(uniq.begin(), uniq.end());
}

static std::set<int> collect_owned_hero(Il2CppObject *list) {
    std::set<int> s;
    int n = collection_count(list);
    for (int i = 0; i < n; ++i) {
        Il2CppObject *v = collection_item(list, i);
        if (v) s.insert(boxed_i32(v, -1));
    }
    s.erase(-1);
    return s;
}

static std::set<int> collect_owned_cards(Il2CppObject *list) {
    std::set<int> s;
    int n = collection_count(list);
    for (int i = 0; i < n; ++i) {
        Il2CppObject *row = collection_item(list, i);
        int id = 0;
        if (row && read_i32_member(row, "ItemId", id)) s.insert(id);
    }
    return s;
}

static std::vector<int> diff(const std::vector<int> &master, const std::set<int> &owned) {
    std::vector<int> r;
    for (int id : master) if (!owned.count(id)) r.push_back(id);
    return r;
}

static std::vector<int> merge_unique(const std::vector<int> &a, const std::vector<int> &b) {
    std::set<int> s(a.begin(), a.end());
    s.insert(b.begin(), b.end());
    return std::vector<int>(s.begin(), s.end());
}

static bool invoke_one_obj(Il2CppObject *receiver, const char *method_name, Il2CppObject *arg) {
    if (!receiver) return false;
    const MethodInfo *m = find_method(receiver->klass, method_name, 1);
    if (!m) return false;
    void *p[1] = {arg};
    return invoke_checked(m, receiver, p, nullptr);
}

static bool run_unlock_once(const char *game_data_dir) {
    g_status_path = std::string(game_data_dir) + "/files/inkvasion_standalone.log";
    {
        std::ofstream f(g_status_path, std::ios::trunc);
        if (f.good()) f << "[INKVASION STANDALONE V1] START\n";
    }

    Il2CppClass *payment_class = find_class_short("GamePaymentManager");
    Il2CppClass *external_class = find_class_short("ExternalConfigManager");
    Il2CppClass *user_card_class = find_class_short("UserCard", "ChillyRoom.Services.BuildingGame");
    Il2CppClass *card_change_class = find_class_short("CardChange", "ChillyRoom.Services.BuildingGame");

    if (!payment_class || !external_class || !user_card_class || !card_change_class) {
        status("FATAL | required type missing");
        return false;
    }

    Il2CppObject *payment = nullptr;
    Il2CppObject *external = nullptr;
    Il2CppObject *hero_master_list = nullptr;
    Il2CppObject *fight_master_list = nullptr;
    Il2CppObject *build_master_list = nullptr;
    Il2CppObject *hero_list = nullptr;
    Il2CppObject *card_list = nullptr;

    for (int sec = 0; sec < 120; ++sec) {
        if (!payment) payment = find_live(payment_class);
        if (!external) external = find_live(external_class);

        if (payment && external) {
            call0_class(external_class, external, "GetHeroConfigList", &hero_master_list);
            call0_class(payment_class, payment, "get_FightCardItemConfigList", &fight_master_list);
            call0_class(payment_class, payment, "get_BuildingCardItemConfigList", &build_master_list);
            call0_class(payment_class, payment, "get_HeroList", &hero_list);
            call0_class(payment_class, payment, "get_CardList", &card_list);

            int hc = collection_count(hero_master_list);
            int fc = collection_count(fight_master_list);
            int bc = collection_count(build_master_list);
            int oh = collection_count(hero_list);
            int oc = collection_count(card_list);

            if (hc > 0 && fc > 0 && bc > 0 && oh > 0 && oc >= 0) {
                status("READY | asset + master initialized");
                break;
            }
        }

        hero_master_list = fight_master_list = build_master_list = nullptr;
        hero_list = card_list = nullptr;
        sleep(1);
    }

    if (!payment || !external || !hero_master_list || !fight_master_list ||
        !build_master_list || !hero_list || !card_list) {
        status("FATAL | initialization timeout");
        return false;
    }

    auto hero_master = collect_ids(hero_master_list, "id");
    auto fight_master = collect_ids(fight_master_list, "id");
    auto build_master = collect_ids(build_master_list, "id");
    auto owned_hero = collect_owned_hero(hero_list);
    auto owned_cards = collect_owned_cards(card_list);

    if (hero_master.empty() || fight_master.empty() || build_master.empty()) {
        status("FATAL | master scan empty");
        return false;
    }

    auto missing_hero = diff(hero_master, owned_hero);
    auto missing_fight = diff(fight_master, owned_cards);
    auto missing_build = diff(build_master, owned_cards);
    auto missing_cards = merge_unique(missing_fight, missing_build);

    status("MASTER | hero=" + std::to_string(hero_master.size()) +
           " fight=" + std::to_string(fight_master.size()) +
           " build=" + std::to_string(build_master.size()));
    status("MISSING | hero=" + std::to_string(missing_hero.size()) +
           " fight=" + std::to_string(missing_fight.size()) +
           " build=" + std::to_string(missing_build.size()));

    if (!missing_cards.empty()) {
        Il2CppObject *change = new_object(card_change_class);
        Il2CppObject *add_list = make_generic_list(user_card_class);
        Il2CppObject *modify_list = make_generic_list(user_card_class);
        if (!change || !add_list || !modify_list) {
            status("FATAL | CardChange/List<UserCard> create failed");
            return false;
        }

        for (int id : missing_cards) {
            Il2CppObject *uc = new_object(user_card_class);
            if (!uc ||
                !set_i32_property(uc, "CardId", id) ||
                !set_i32_property(uc, "CardLevel", 1) ||
                !set_i32_property(uc, "CardExp", 0) ||
                !list_add_obj(add_list, uc)) {
                status("FATAL | UserCard build failed id=" + std::to_string(id));
                return false;
            }
        }

        if (!set_obj_property(change, "AddList", add_list) ||
            !set_obj_property(change, "ModifyList", modify_list) ||
            !invoke_one_obj(payment, "UnpackCardChange", change)) {
            status("FATAL | UnpackCardChange failed");
            return false;
        }

        status("CARD APPLY | PASS | add=" + std::to_string(missing_cards.size()));
    }

    if (!missing_hero.empty()) {
        Il2CppObject *adds = new_same_list(hero_list);
        if (!adds) {
            status("FATAL | List<int> create failed");
            return false;
        }
        for (int id : missing_hero) {
            if (!list_add_i32(adds, id)) {
                status("FATAL | hero list Add failed id=" + std::to_string(id));
                return false;
            }
        }
        if (!invoke_one_obj(payment, "UnpackHeroChange", adds)) {
            status("FATAL | UnpackHeroChange failed");
            return false;
        }
        status("HERO APPLY | PASS | add=" + std::to_string(missing_hero.size()));
    }

    Il2CppObject *hero_after = nullptr;
    Il2CppObject *card_after = nullptr;
    call0_class(payment_class, payment, "get_HeroList", &hero_after);
    call0_class(payment_class, payment, "get_CardList", &card_after);

    auto h2 = collect_owned_hero(hero_after);
    auto c2 = collect_owned_cards(card_after);
    size_t rh = diff(hero_master, h2).size();
    size_t rf = diff(fight_master, c2).size();
    size_t rb = diff(build_master, c2).size();

    status("VERIFY | heroRemain=" + std::to_string(rh) +
           " fightRemain=" + std::to_string(rf) +
           " buildRemain=" + std::to_string(rb));

    if (rh == 0 && rf == 0 && rb == 0) {
        status("RESULT | PASS | ALL HERO + FIGHT + BUILD");
        return true;
    }

    status("RESULT | PARTIAL");
    return false;
}

static void hack_start(const char *game_data_dir) {
    bool load = false;
    for (int i = 0; i < 90; ++i) {
        void *handle = xdl_open("libil2cpp.so", 0);
        if (handle) {
            load = true;
            il2cpp_api_init(handle);
            run_unlock_once(game_data_dir);
            break;
        }
        sleep(1);
    }
    if (!load) LOGE("[INKVASION AUTO] libil2cpp.so not found");
}

std::string GetLibDir(JavaVM *vms) {
    JNIEnv *env = nullptr;
    vms->AttachCurrentThread(&env, nullptr);
    jclass activity_thread_clz = env->FindClass("android/app/ActivityThread");
    if (activity_thread_clz != nullptr) {
        jmethodID currentApplicationId = env->GetStaticMethodID(activity_thread_clz,
                                                                "currentApplication",
                                                                "()Landroid/app/Application;");
        if (currentApplicationId) {
            jobject application = env->CallStaticObjectMethod(activity_thread_clz,
                                                              currentApplicationId);
            jclass application_clazz = env->GetObjectClass(application);
            if (application_clazz) {
                jmethodID get_application_info = env->GetMethodID(application_clazz,
                                                                  "getApplicationInfo",
                                                                  "()Landroid/content/pm/ApplicationInfo;");
                if (get_application_info) {
                    jobject application_info = env->CallObjectMethod(application,
                                                                     get_application_info);
                    jfieldID native_library_dir_id = env->GetFieldID(
                            env->GetObjectClass(application_info), "nativeLibraryDir",
                            "Ljava/lang/String;");
                    if (native_library_dir_id) {
                        auto native_library_dir_jstring = (jstring) env->GetObjectField(
                                application_info, native_library_dir_id);
                        auto path = env->GetStringUTFChars(native_library_dir_jstring, nullptr);
                        std::string lib_dir(path);
                        env->ReleaseStringUTFChars(native_library_dir_jstring, path);
                        return lib_dir;
                    }
                }
            }
        }
    }
    return {};
}

static std::string GetNativeBridgeLibrary() {
    auto value = std::array<char, PROP_VALUE_MAX>();
    __system_property_get("ro.dalvik.vm.native.bridge", value.data());
    return {value.data()};
}

struct NativeBridgeCallbacks {
    uint32_t version;
    void *initialize;
    void *(*loadLibrary)(const char *libpath, int flag);
    void *(*getTrampoline)(void *handle, const char *name, const char *shorty, uint32_t len);
    void *isSupported;
    void *getAppEnv;
    void *isCompatibleWith;
    void *getSignalHandler;
    void *unloadLibrary;
    void *getError;
    void *isPathSupported;
    void *initAnonymousNamespace;
    void *createNamespace;
    void *linkNamespaces;
    void *(*loadLibraryExt)(const char *libpath, int flag, void *ns);
};

static bool NativeBridgeLoad(const char *game_data_dir, int api_level, void *data, size_t length) {
    sleep(5);

    auto libart = dlopen("libart.so", RTLD_NOW);
    if (!libart) return false;
    auto JNI_GetCreatedJavaVMs = (jint (*)(JavaVM **, jsize, jsize *))
            dlsym(libart, "JNI_GetCreatedJavaVMs");
    if (!JNI_GetCreatedJavaVMs) return false;

    JavaVM *vms_buf[1];
    JavaVM *vms = nullptr;
    jsize num_vms = 0;
    if (JNI_GetCreatedJavaVMs(vms_buf, 1, &num_vms) != JNI_OK || num_vms <= 0) return false;
    vms = vms_buf[0];

    auto lib_dir = GetLibDir(vms);
    if (lib_dir.empty()) return false;
    if (lib_dir.find("/lib/x86") != std::string::npos) {
        if (data && length) munmap(data, length);
        return false;
    }

    auto nb = dlopen("libhoudini.so", RTLD_NOW);
    if (!nb) {
        auto native_bridge = GetNativeBridgeLibrary();
        if (!native_bridge.empty()) nb = dlopen(native_bridge.data(), RTLD_NOW);
    }
    if (!nb) return false;

    auto callbacks = (NativeBridgeCallbacks *) dlsym(nb, "NativeBridgeItf");
    if (!callbacks || !data || !length) return false;

    int fd = syscall(__NR_memfd_create, "inkvasion_auto", MFD_CLOEXEC);
    if (fd < 0) return false;
    if (ftruncate(fd, (off_t) length) != 0) { close(fd); return false; }

    void *mem = mmap(nullptr, length, PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) { close(fd); return false; }
    std::memcpy(mem, data, length);
    munmap(mem, length);
    munmap(data, length);

    char path[PATH_MAX];
    std::snprintf(path, PATH_MAX, "/proc/self/fd/%d", fd);

    void *arm_handle = api_level >= 26
            ? callbacks->loadLibraryExt(path, RTLD_NOW, (void *) 3)
            : callbacks->loadLibrary(path, RTLD_NOW);

    if (!arm_handle) { close(fd); return false; }

    auto init = (void (*)(JavaVM *, void *))
            callbacks->getTrampoline(arm_handle, "JNI_OnLoad", nullptr, 0);
    if (!init) { close(fd); return false; }

    init(vms, (void *) game_data_dir);
    return true;
}

void hack_prepare(const char *game_data_dir, void *data, size_t length) {
    int api_level = android_get_device_api_level();
#if defined(__i386__) || defined(__x86_64__)
    if (!NativeBridgeLoad(game_data_dir, api_level, data, length)) {
#endif
        hack_start(game_data_dir);
#if defined(__i386__) || defined(__x86_64__)
    }
#endif
}

#if defined(__arm__) || defined(__aarch64__)
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    auto game_data_dir = (const char *) reserved;
    std::thread hack_thread(hack_start, game_data_dir);
    hack_thread.detach();
    return JNI_VERSION_1_6;
}
#endif
