#include <jni.h>
#include <android/log.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <set>
#include <fstream>
#include <thread>
#include <algorithm>

#include "zygisk.hpp"
#include "xdl.h"
#include "il2cpp-tabledefs.h"
#include "il2cpp-class.h"

#define LOG_TAG "InkvasionStandalone"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define DO_API(r, n, p) r (*n) p = nullptr
#include "il2cpp-api-functions.h"
#undef DO_API

static const char *kPackage = "com.chillyroom.inkvasion.gp";
static std::string g_log_path;

static void slog(const std::string &s) {
    LOGI("%s", s.c_str());
    if (!g_log_path.empty()) {
        std::ofstream f(g_log_path, std::ios::app);
        if (f.good()) f << s << "\n";
    }
}

static bool init_api(void *handle) {
#define DO_API(r, n, p) n = reinterpret_cast<r (*) p>(xdl_sym(handle, #n, nullptr));
#include "il2cpp-api-functions.h"
#undef DO_API
    return il2cpp_domain_get &&
           il2cpp_domain_get_assemblies &&
           il2cpp_assembly_get_image &&
           il2cpp_image_get_class_count &&
           il2cpp_image_get_class &&
           il2cpp_class_get_name &&
           il2cpp_class_get_namespace &&
           il2cpp_class_get_parent &&
           il2cpp_class_get_methods &&
           il2cpp_class_get_field_from_name &&
           il2cpp_class_get_properties &&
           il2cpp_class_from_name &&
           il2cpp_class_from_type &&
           il2cpp_class_get_type &&
           il2cpp_method_get_name &&
           il2cpp_method_get_param_count &&
           il2cpp_method_get_param &&
           il2cpp_property_get_name &&
           il2cpp_property_get_get_method &&
           il2cpp_runtime_invoke &&
           il2cpp_object_new &&
           il2cpp_runtime_object_init &&
           il2cpp_object_unbox &&
           il2cpp_field_get_value &&
           il2cpp_get_corlib &&
           il2cpp_type_get_object &&
           il2cpp_array_new &&
           il2cpp_class_from_system_type &&
           il2cpp_thread_attach;
}

struct Root {
    uint32_t h = 0;
    explicit Root(Il2CppObject *o) {
        if (o && il2cpp_gchandle_new) h = il2cpp_gchandle_new(o, true);
    }
    ~Root() {
        if (h && il2cpp_gchandle_free) il2cpp_gchandle_free(h);
    }
    Root(const Root &) = delete;
    Root &operator=(const Root &) = delete;
};

static bool invoke0(const MethodInfo *m, void *obj, Il2CppObject **out = nullptr) {
    if (!m) return false;
    Il2CppException *exc = nullptr;
    Il2CppObject *r = il2cpp_runtime_invoke(m, obj, nullptr, &exc);
    if (exc) {
        LOGE("managed exception: %s", il2cpp_method_get_name(m));
        return false;
    }
    if (out) *out = r;
    return true;
}

static bool invoke_i32(const MethodInfo *m, void *obj, int32_t v, Il2CppObject **out = nullptr) {
    if (!m) return false;
    void *args[1] = {&v};
    Il2CppException *exc = nullptr;
    Il2CppObject *r = il2cpp_runtime_invoke(m, obj, args, &exc);
    if (exc) {
        LOGE("managed exception: %s", il2cpp_method_get_name(m));
        return false;
    }
    if (out) *out = r;
    return true;
}

static bool invoke_ref(const MethodInfo *m, void *obj, Il2CppObject *arg, Il2CppObject **out = nullptr) {
    if (!m) return false;
    Il2CppObject *tmp = arg;
    void *args[1] = {&tmp};
    Il2CppException *exc = nullptr;
    Il2CppObject *r = il2cpp_runtime_invoke(m, obj, args, &exc);
    if (exc) {
        LOGE("managed exception: %s", il2cpp_method_get_name(m));
        return false;
    }
    if (out) *out = r;
    return true;
}

static const MethodInfo *find_method(Il2CppClass *klass, const char *name, int argc) {
    for (Il2CppClass *k = klass; k; k = il2cpp_class_get_parent(k)) {
        void *iter = nullptr;
        while (const MethodInfo *m = il2cpp_class_get_methods(k, &iter)) {
            const char *n = il2cpp_method_get_name(m);
            if (n && std::strcmp(n, name) == 0 &&
                static_cast<int>(il2cpp_method_get_param_count(m)) == argc) {
                return m;
            }
        }
    }
    return nullptr;
}

static FieldInfo *find_field(Il2CppClass *klass, const char *name) {
    for (Il2CppClass *k = klass; k; k = il2cpp_class_get_parent(k)) {
        FieldInfo *f = il2cpp_class_get_field_from_name(k, name);
        if (f) return f;
    }
    return nullptr;
}

static PropertyInfo *find_property(Il2CppClass *klass, const char *name) {
    for (Il2CppClass *k = klass; k; k = il2cpp_class_get_parent(k)) {
        void *iter = nullptr;
        while (const PropertyInfo *pc = il2cpp_class_get_properties(k, &iter)) {
            auto *p = const_cast<PropertyInfo *>(pc);
            const char *n = il2cpp_property_get_name(p);
            if (n && std::strcmp(n, name) == 0) return p;
        }
    }
    return nullptr;
}

static Il2CppClass *find_class(const char *short_name, const char *ns_contains = nullptr) {
    size_t n = 0;
    auto domain = il2cpp_domain_get();
    auto assemblies = il2cpp_domain_get_assemblies(domain, &n);
    for (size_t i = 0; i < n; ++i) {
        auto image = il2cpp_assembly_get_image(assemblies[i]);
        if (!image) continue;
        size_t count = il2cpp_image_get_class_count(image);
        for (size_t j = 0; j < count; ++j) {
            auto *k = const_cast<Il2CppClass *>(il2cpp_image_get_class(image, j));
            if (!k) continue;
            const char *name = il2cpp_class_get_name(k);
            if (!name || std::strcmp(name, short_name) != 0) continue;
            if (ns_contains && *ns_contains) {
                const char *ns = il2cpp_class_get_namespace(k);
                if (!ns || !std::strstr(ns, ns_contains)) continue;
            }
            return k;
        }
    }
    return nullptr;
}

struct LiveCtx {
    std::vector<Il2CppObject *> objects;
};

static void live_cb(Il2CppObject **arr, int size, void *ud) {
    auto *ctx = reinterpret_cast<LiveCtx *>(ud);
    if (!ctx || !arr) return;
    for (int i = 0; i < size; ++i) if (arr[i]) ctx->objects.push_back(arr[i]);
}

static void *live_realloc(void *ptr, size_t size, void *) {
    return std::realloc(ptr, size);
}

static Il2CppObject *find_live_liveness(Il2CppClass *klass) {
    if (!klass ||
        !il2cpp_unity_liveness_allocate_struct ||
        !il2cpp_unity_liveness_calculation_from_statics ||
        !il2cpp_unity_liveness_finalize ||
        !il2cpp_unity_liveness_free_struct) {
        return nullptr;
    }

    LiveCtx ctx;
    void *state = il2cpp_unity_liveness_allocate_struct(
        klass, 4096, live_cb, &ctx, live_realloc);
    if (!state) return nullptr;

    il2cpp_unity_liveness_calculation_from_statics(state);
    il2cpp_unity_liveness_finalize(state);
    il2cpp_unity_liveness_free_struct(state);

    return ctx.objects.empty() ? nullptr : ctx.objects.front();
}

static Il2CppClass *find_unity_class(const char *name) {
    size_t n = 0;
    auto domain = il2cpp_domain_get();
    auto assemblies = il2cpp_domain_get_assemblies(domain, &n);
    for (size_t i = 0; i < n; ++i) {
        auto image = il2cpp_assembly_get_image(assemblies[i]);
        if (!image) continue;
        Il2CppClass *k = il2cpp_class_from_name(image, "UnityEngine", name);
        if (k) return k;
    }
    return nullptr;
}

static Il2CppObject *first_from_array(Il2CppObject *obj) {
    auto *arr = reinterpret_cast<Il2CppArray *>(obj);
    if (!arr || arr->max_length == 0) return nullptr;
    auto **items = reinterpret_cast<Il2CppObject **>(arr->vector);
    for (size_t i = 0; i < arr->max_length; ++i) if (items[i]) return items[i];
    return nullptr;
}

static Il2CppObject *find_live_unity(Il2CppClass *target) {
    if (!target || !il2cpp_type_get_object || !il2cpp_class_get_type) return nullptr;
    Il2CppClass *uo = find_unity_class("Object");
    if (!uo) return nullptr;

    const MethodInfo *candidate = nullptr;
    void *it = nullptr;
    while (const MethodInfo *m = il2cpp_class_get_methods(uo, &it)) {
        const char *n = il2cpp_method_get_name(m);
        if (!n || std::strcmp(n, "FindObjectsOfType") != 0 ||
            il2cpp_method_get_param_count(m) != 1) continue;
        Il2CppClass *pk = il2cpp_class_from_type(il2cpp_method_get_param(m, 0));
        if (pk && std::strcmp(il2cpp_class_get_name(pk), "Type") == 0) {
            candidate = m;
            break;
        }
    }
    if (!candidate) return nullptr;

    Il2CppObject *type_obj = il2cpp_type_get_object(il2cpp_class_get_type(target));
    Il2CppObject *result = nullptr;
    if (!invoke_ref(candidate, nullptr, type_obj, &result)) return nullptr;
    return first_from_array(result);
}

static Il2CppObject *find_live(Il2CppClass *klass) {
    Il2CppObject *o = find_live_liveness(klass);
    return o ? o : find_live_unity(klass);
}

static int unbox_i32(Il2CppObject *o, int fallback = -1) {
    if (!o || !il2cpp_object_unbox) return fallback;
    void *p = il2cpp_object_unbox(o);
    return p ? *reinterpret_cast<int32_t *>(p) : fallback;
}

static bool call0(Il2CppObject *obj, const char *name, Il2CppObject **out) {
    if (!obj) return false;
    return invoke0(find_method(obj->klass, name, 0), obj, out);
}

static int count_of(Il2CppObject *list) {
    Il2CppObject *r = nullptr;
    if (!list || !call0(list, "get_Count", &r)) return -1;
    return unbox_i32(r, -1);
}

static Il2CppObject *item_at(Il2CppObject *list, int idx) {
    if (!list) return nullptr;
    Il2CppObject *r = nullptr;
    if (!invoke_i32(find_method(list->klass, "get_Item", 1), list, idx, &r)) return nullptr;
    return r;
}

static bool read_i32(Il2CppObject *obj, const char *name, int &out) {
    if (!obj) return false;

    if (FieldInfo *f = find_field(obj->klass, name)) {
        int32_t v = 0;
        il2cpp_field_get_value(obj, f, &v);
        out = v;
        return true;
    }

    if (PropertyInfo *p = find_property(obj->klass, name)) {
        const MethodInfo *g = il2cpp_property_get_get_method(p);
        Il2CppObject *r = nullptr;
        if (g && invoke0(g, obj, &r) && r) {
            out = unbox_i32(r, 0);
            return true;
        }
    }

    std::string getter = std::string("get_") + name;
    Il2CppObject *r = nullptr;
    if (invoke0(find_method(obj->klass, getter.c_str(), 0), obj, &r) && r) {
        out = unbox_i32(r, 0);
        return true;
    }

    return false;
}

static bool set_i32(Il2CppObject *obj, const char *name, int32_t value) {
    if (!obj) return false;
    std::string setter = std::string("set_") + name;
    return invoke_i32(find_method(obj->klass, setter.c_str(), 1), obj, value, nullptr);
}

static bool list_add_i32(Il2CppObject *list, int32_t value) {
    return list && invoke_i32(find_method(list->klass, "Add", 1), list, value, nullptr);
}

static bool list_add_ref(Il2CppObject *list, Il2CppObject *value) {
    return list && value &&
           invoke_ref(find_method(list->klass, "Add", 1), list, value, nullptr);
}

static Il2CppObject *new_object(Il2CppClass *klass) {
    if (!klass) return nullptr;
    Il2CppObject *o = il2cpp_object_new(klass);
    if (!o) return nullptr;
    il2cpp_runtime_object_init(o);
    return o;
}

static Il2CppObject *new_list_like(Il2CppObject *seed) {
    return seed ? new_object(seed->klass) : nullptr;
}

static Il2CppObject *make_list(Il2CppClass *element) {
    if (!element) return nullptr;
    const Il2CppImage *corlib = il2cpp_get_corlib();
    if (!corlib) return nullptr;

    Il2CppClass *list_def =
        il2cpp_class_from_name(corlib, "System.Collections.Generic", "List\`1");
    Il2CppClass *type_class =
        il2cpp_class_from_name(corlib, "System", "Type");
    if (!list_def || !type_class) return nullptr;

    Il2CppObject *open_type =
        il2cpp_type_get_object(il2cpp_class_get_type(list_def));
    Il2CppObject *arg_type =
        il2cpp_type_get_object(il2cpp_class_get_type(element));
    if (!open_type || !arg_type) return nullptr;

    Il2CppArray *args = il2cpp_array_new(type_class, 1);
    if (!args) return nullptr;
    reinterpret_cast<Il2CppObject **>(args->vector)[0] = arg_type;

    const MethodInfo *make =
        find_method(open_type->klass, "MakeGenericType", 1);
    Il2CppObject *closed_type = nullptr;
    if (!invoke_ref(make, open_type, reinterpret_cast<Il2CppObject *>(args), &closed_type) ||
        !closed_type) {
        return nullptr;
    }

    Il2CppClass *closed = il2cpp_class_from_system_type(
        reinterpret_cast<Il2CppReflectionType *>(closed_type));
    return new_object(closed);
}

static bool set_ref(Il2CppObject *obj, const char *name, Il2CppObject *value) {
    if (!obj) return false;
    std::string setter = std::string("set_") + name;
    return invoke_ref(find_method(obj->klass, setter.c_str(), 1), obj, value, nullptr);
}

static std::vector<int> master_ids(Il2CppObject *list, const char *member) {
    std::set<int> s;
    int n = count_of(list);
    for (int i = 0; i < n; ++i) {
        Il2CppObject *row = item_at(list, i);
        int id = 0;
        if (row && read_i32(row, member, id)) s.insert(id);
    }
    return {s.begin(), s.end()};
}

static std::set<int> owned_heroes(Il2CppObject *list) {
    std::set<int> s;
    int n = count_of(list);
    for (int i = 0; i < n; ++i) {
        Il2CppObject *v = item_at(list, i);
        int id = unbox_i32(v, -1);
        if (id >= 0) s.insert(id);
    }
    return s;
}

static std::set<int> owned_cards(Il2CppObject *list) {
    std::set<int> s;
    int n = count_of(list);
    for (int i = 0; i < n; ++i) {
        Il2CppObject *row = item_at(list, i);
        int id = 0;
        if (row && read_i32(row, "ItemId", id)) s.insert(id);
    }
    return s;
}

static std::vector<int> missing(const std::vector<int> &master, const std::set<int> &owned) {
    std::vector<int> r;
    for (int id : master) if (!owned.count(id)) r.push_back(id);
    return r;
}

static std::vector<int> merge_ids(const std::vector<int> &a, const std::vector<int> &b) {
    std::set<int> s(a.begin(), a.end());
    s.insert(b.begin(), b.end());
    return {s.begin(), s.end()};
}

struct GameRefs {
    Il2CppClass *payment_class = nullptr;
    Il2CppClass *external_class = nullptr;
    Il2CppClass *user_card_class = nullptr;
    Il2CppClass *card_change_class = nullptr;
    Il2CppObject *payment = nullptr;
    Il2CppObject *external = nullptr;
};

static bool resolve_refs(GameRefs &g) {
    if (!g.payment_class) g.payment_class = find_class("GamePaymentManager");
    if (!g.external_class) g.external_class = find_class("ExternalConfigManager", "GameExternal");
    if (!g.user_card_class) g.user_card_class =
        find_class("UserCard", "ChillyRoom.Services.BuildingGame");
    if (!g.card_change_class) g.card_change_class =
        find_class("CardChange", "ChillyRoom.Services.BuildingGame");

    if (!g.payment_class || !g.external_class ||
        !g.user_card_class || !g.card_change_class) return false;

    if (!g.payment) g.payment = find_live(g.payment_class);
    if (!g.external) g.external = find_live(g.external_class);
    return g.payment && g.external;
}

static bool get_lists(GameRefs &g,
                      Il2CppObject **hero_master,
                      Il2CppObject **fight_master,
                      Il2CppObject **build_master,
                      Il2CppObject **hero_list,
                      Il2CppObject **card_list) {
    *hero_master = *fight_master = *build_master = *hero_list = *card_list = nullptr;

    if (!call0(g.external, "GetHeroConfigList", hero_master)) return false;
    call0(g.payment, "get_FightCardItemConfigList", fight_master);
    call0(g.payment, "get_BuildingCardItemConfigList", build_master);

    if (count_of(*fight_master) <= 0 || count_of(*build_master) <= 0) {
        invoke0(find_method(g.payment->klass, "LoadConfigs", 0), g.payment, nullptr);
        call0(g.payment, "get_FightCardItemConfigList", fight_master);
        call0(g.payment, "get_BuildingCardItemConfigList", build_master);
    }

    if (!call0(g.payment, "get_HeroList", hero_list)) return false;
    if (!call0(g.payment, "get_CardList", card_list)) return false;

    return count_of(*hero_master) > 0 &&
           count_of(*fight_master) > 0 &&
           count_of(*build_master) > 0 &&
           count_of(*hero_list) > 0 &&
           count_of(*card_list) > 0;
}

static bool apply_once(GameRefs &g, bool verbose) {
    Il2CppObject *hero_master = nullptr;
    Il2CppObject *fight_master = nullptr;
    Il2CppObject *build_master = nullptr;
    Il2CppObject *hero_list = nullptr;
    Il2CppObject *card_list = nullptr;

    if (!get_lists(g, &hero_master, &fight_master, &build_master, &hero_list, &card_list)) {
        slog("APPLY | lists unavailable");
        return false;
    }

    auto hm = master_ids(hero_master, "id");
    auto fm = master_ids(fight_master, "id");
    auto bm = master_ids(build_master, "id");
    auto oh = owned_heroes(hero_list);
    auto oc = owned_cards(card_list);

    auto mh = missing(hm, oh);
    auto mf = missing(fm, oc);
    auto mb = missing(bm, oc);
    auto mc = merge_ids(mf, mb);

    if (verbose) {
        slog("MASTER | hero=" + std::to_string(hm.size()) +
             " fight=" + std::to_string(fm.size()) +
             " build=" + std::to_string(bm.size()));
        slog("MISSING | hero=" + std::to_string(mh.size()) +
             " fight=" + std::to_string(mf.size()) +
             " build=" + std::to_string(mb.size()));
    }

    if (!mc.empty()) {
        Il2CppObject *change = new_object(g.card_change_class);
        Il2CppObject *add_list = make_list(g.user_card_class);
        Il2CppObject *modify_list = make_list(g.user_card_class);
        if (!change || !add_list || !modify_list) {
            slog("FATAL | CardChange/List<UserCard> create failed");
            return false;
        }

        Root r_change(change), r_add(add_list), r_modify(modify_list);

        for (int id : mc) {
            Il2CppObject *uc = new_object(g.user_card_class);
            if (!uc) {
                slog("FATAL | UserCard create failed");
                return false;
            }
            Root r_uc(uc);

            if (!set_i32(uc, "CardId", id) ||
                !set_i32(uc, "CardLevel", 1) ||
                !set_i32(uc, "CardExp", 0) ||
                !list_add_ref(add_list, uc)) {
                slog("FATAL | UserCard build failed id=" + std::to_string(id));
                return false;
            }
        }

        if (!set_ref(change, "AddList", add_list) ||
            !set_ref(change, "ModifyList", modify_list) ||
            !invoke_ref(find_method(g.payment->klass, "UnpackCardChange", 1),
                        g.payment, change, nullptr)) {
            slog("FATAL | UnpackCardChange failed");
            return false;
        }

        slog("CARD APPLY | PASS | add=" + std::to_string(mc.size()));
    }

    if (!mh.empty()) {
        Il2CppObject *adds = new_list_like(hero_list);
        if (!adds) {
            slog("FATAL | List<int> create failed");
            return false;
        }
        Root r_adds(adds);

        for (int id : mh) {
            if (!list_add_i32(adds, id)) {
                slog("FATAL | Hero Add failed id=" + std::to_string(id));
                return false;
            }
        }

        if (!invoke_ref(find_method(g.payment->klass, "UnpackHeroChange", 1),
                        g.payment, adds, nullptr)) {
            slog("FATAL | UnpackHeroChange failed");
            return false;
        }

        slog("HERO APPLY | PASS | add=" + std::to_string(mh.size()));
    }

    Il2CppObject *hero_after = nullptr;
    Il2CppObject *card_after = nullptr;
    call0(g.payment, "get_HeroList", &hero_after);
    call0(g.payment, "get_CardList", &card_after);

    auto h2 = owned_heroes(hero_after);
    auto c2 = owned_cards(card_after);
    size_t rh = missing(hm, h2).size();
    size_t rf = missing(fm, c2).size();
    size_t rb = missing(bm, c2).size();

    slog("VERIFY | heroRemain=" + std::to_string(rh) +
         " fightRemain=" + std::to_string(rf) +
         " buildRemain=" + std::to_string(rb));

    return rh == 0 && rf == 0 && rb == 0;
}

static void worker(std::string data_dir) {
    g_log_path = data_dir + "/files/inkvasion_standalone.log";
    {
        std::ofstream f(g_log_path, std::ios::trunc);
        if (f.good()) f << "[INKVASION STANDALONE V1] START\n";
    }

    void *handle = nullptr;
    for (int i = 0; i < 90 && !handle; ++i) {
        handle = xdl_open("libil2cpp.so", 0);
        if (!handle) sleep(1);
    }
    if (!handle) {
        slog("FATAL | libil2cpp.so timeout");
        return;
    }

    if (!init_api(handle)) {
        slog("FATAL | IL2CPP API init failed");
        return;
    }

    Il2CppDomain *domain = nullptr;
    for (int i = 0; i < 90 && !domain; ++i) {
        domain = il2cpp_domain_get();
        if (!domain) sleep(1);
    }
    if (!domain) {
        slog("FATAL | domain unavailable");
        return;
    }

    Il2CppThread *attached = il2cpp_thread_attach(domain);
    if (!attached) {
        slog("FATAL | il2cpp_thread_attach failed");
        return;
    }

    GameRefs g;
    bool ready = false;
    for (int i = 0; i < 120; ++i) {
        if (resolve_refs(g)) {
            Il2CppObject *hm = nullptr, *fm = nullptr, *bm = nullptr, *hl = nullptr, *cl = nullptr;
            if (get_lists(g, &hm, &fm, &bm, &hl, &cl)) {
                ready = true;
                break;
            }
        }
        sleep(1);
    }

    if (!ready) {
        slog("FATAL | game asset/master init timeout");
        return;
    }

    slog("READY | GamePaymentManager + master");
    sleep(8);

    Root r_payment(g.payment), r_external(g.external);

    bool pass1 = apply_once(g, true);
    slog(pass1 ? "PASS1 | complete" : "PASS1 | incomplete");

    sleep(15);

    bool pass2 = apply_once(g, false);
    slog(pass2 ? "RESULT | PASS | ALL HERO + FIGHT + BUILD"
               : "RESULT | PARTIAL");

    slog("THREAD | EXIT");
}

class InkvasionModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        api_ = api;
        env_ = env;
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        enabled_ = false;
        if (!args || !args->nice_name || !args->app_data_dir) return;

        const char *name = env_->GetStringUTFChars(args->nice_name, nullptr);
        const char *dir = env_->GetStringUTFChars(args->app_data_dir, nullptr);

        if (name && std::strcmp(name, kPackage) == 0) {
            enabled_ = true;
            data_dir_ = dir ? dir : "";
            LOGI("target process detected: %s", name);
        } else {
            api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
        }

        if (name) env_->ReleaseStringUTFChars(args->nice_name, name);
        if (dir) env_->ReleaseStringUTFChars(args->app_data_dir, dir);
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *) override {
        if (!enabled_) return;
        std::thread t(worker, data_dir_);
        t.detach();
    }

private:
    zygisk::Api *api_ = nullptr;
    JNIEnv *env_ = nullptr;
    bool enabled_ = false;
    std::string data_dir_;
};

REGISTER_ZYGISK_MODULE(InkvasionModule)
