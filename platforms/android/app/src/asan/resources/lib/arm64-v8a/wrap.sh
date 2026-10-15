#!/system/bin/sh
# AddressSanitizer builds (-Pmelee.asan=true): preload the ASan runtime before
# the app starts. Reports go to logcat (tag asan or the app's).
HERE="$(cd "$(dirname "$0")" && pwd)"
# replace_intrin=0: no overlap checks in memcpy and friends (the decomp copies
# overlapping ranges, e.g. synth.c HSD_SynthSFXSampleLoadCallback).
export ASAN_OPTIONS=log_to_syslog=true,allow_user_segv_handler=1,detect_container_overflow=0,halt_on_error=1,malloc_context_size=30,replace_intrin=0
export LD_PRELOAD="$(ls "$HERE"/libclang_rt.asan-*-android.so)"
exec "$@"
