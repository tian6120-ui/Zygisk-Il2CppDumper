from pathlib import Path
import sys

root = Path(sys.argv[1] if len(sys.argv) > 1 else "Il2CppTool")
main = root / "app/src/main/jni/Main.cpp"
mk = root / "app/src/main/jni/Android.mk"
appmk = root / "app/src/main/jni/Application.mk"

s = main.read_text(encoding="utf-8")

# Branding
s = s.replace('const char *title = OBFUSCATE("Il2CppTool v0.9 | HitMargin");',
              'const char *title = OBFUSCATE("AZ Tool OpenCore");', 1)
s = s.replace('ImGui::Button("bilibili HitMargin", ImVec2(-1, 0))',
              'ImGui::Button("AZ Tool | OpenCore", ImVec2(-1, 0))', 1)
s = s.replace('OBFUSCATE("https://m.bilibili.com/space/1757946676")',
              'OBFUSCATE("AZ Tool")', 1)

# Own config name
s = s.replace('"tool_conf.json"', '"az_tool_conf.json"')

# Add JNI / atomic only if absent.
if '#include <jni.h>' not in s:
    s = s.replace('#include <pthread.h> // for pthread_create',
                  '#include <jni.h>\n#include <atomic>\n#include <pthread.h> // for pthread_create', 1)

# Replace startup tail with idempotent ABI-compatible bootstrap.
anchor = '''// we will run our hacks in a new thread so our while loop doesn't block process main thread
void *hack_thread(void *)
{
    logger::Clear();

    LOGI("pthread created");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    initModMenu((void *)draw_thread, (void *)on_init);
    return nullptr;
}
'''
replacement = r'''// AZ clean-room bootstrap.
static std::atomic<bool> g_azStarted{false};
static JavaVM *g_azVm = nullptr;

void *hack_thread(void *)
{
    logger::Clear();
    LOGI("[AZ] core thread created");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    initModMenu((void *)draw_thread, (void *)on_init);
    return nullptr;
}

static int az_start_once()
{
    bool expected = false;
    if (!g_azStarted.compare_exchange_strong(expected, true))
        return 1;

    pthread_t ptid{};
    if (pthread_create(&ptid, nullptr, hack_thread, nullptr) != 0)
    {
        g_azStarted.store(false);
        LOGE("[AZ] pthread_create failed");
        return 0;
    }
    pthread_detach(ptid);
    LOGI("[AZ] bootstrap scheduled");
    return 1;
}

extern "C" __attribute__((visibility("default")))
int AZ_Start()
{
    return az_start_once();
}

extern "C" __attribute__((visibility("default")))
const char *AZ_GetVersion()
{
    return "AZ-OpenCore-0.1";
}

extern "C" __attribute__((visibility("default")))
const char *AZ_GetCapabilities()
{
    return "imgui,il2cpp,class,method,field,property,enum,object,call,hook,trace,dump,patcher,generic";
}

// V7-compatible public bootstrap shape observed from the reference payload:
// three JNI-sized arguments and integer success result. The implementation here
// is independent and intentionally treats the three values as optional context.
extern "C" __attribute__((visibility("default")))
int B0(JNIEnv *env, jobject activity, jobject classLoader)
{
    (void)activity;
    (void)classLoader;
    if (env && !g_azVm)
        env->GetJavaVM(&g_azVm);
    LOGI("[AZ] B0 called env=%p activity=%p classLoader=%p", env, activity, classLoader);
    return az_start_once();
}

extern "C" __attribute__((visibility("default")))
jint JNI_OnLoad(JavaVM *vm, void *)
{
    g_azVm = vm;
    LOGI("[AZ] JNI_OnLoad vm=%p", vm);
    az_start_once();
    return JNI_VERSION_1_6;
}
'''
if anchor not in s:
    raise SystemExit("bootstrap anchor not found")
s = s.replace(anchor, replacement, 1)

# Constructor delegates to the same idempotent bootstrap.
old_ctor = '''__attribute__((constructor)) void lib_main()
{
    // Create a new thread so it does not block the main thread, means the game would not freeze
    pthread_t ptid;
    pthread_create(&ptid, nullptr, hack_thread, nullptr);
}
'''
new_ctor = '''__attribute__((constructor)) void lib_main()
{
    az_start_once();
}
'''
if old_ctor not in s:
    raise SystemExit("constructor anchor not found")
s = s.replace(old_ctor, new_ctor, 1)

main.write_text(s, encoding="utf-8")

m = mk.read_text(encoding="utf-8")
m = m.replace('LOCAL_MODULE := Il2CppTool', 'LOCAL_MODULE := Tool', 1)
# Keep SONAME exactly libTool.so to make the payload independently loadable
# by generic JNI/dlopen injectors that key off the mature reference ABI.
if '-Wl,-soname,libTool.so' not in m:
    m = m.replace('LOCAL_LDFLAGS += -Wl,--gc-sections,--strip-all,-llog',
                  'LOCAL_LDFLAGS += -Wl,--gc-sections,--strip-all,-llog,-soname,libTool.so', 1)
mk.write_text(m, encoding="utf-8")

a = appmk.read_text(encoding="utf-8")
a = a.replace('APP_PLATFORM := android-28', 'APP_PLATFORM := android-21')
appmk.write_text(a, encoding="utf-8")

print("AZ clean-room core patch applied")
