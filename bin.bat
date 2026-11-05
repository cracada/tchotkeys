@windres app.rc -o app_res.o
gcc -O2 -s -mwindows -finput-charset=UTF-8 -fexec-charset=GBK -o tc_hotkeys.exe tc_hotkeys.c app_res.o -lcomctl32
@rem gcc -O2 -s -mwindows -finput-charset=UTF-8 -fexec-charset=GBK -o TCHotkeys.exe  TCHotkeys.c -lcomctl32
