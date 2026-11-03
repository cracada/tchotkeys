@windres app.rc -o app_res.o
gcc -O2 -s -mwindows -finput-charset=UTF-8 -fexec-charset=GBK -o tchotkeys.exe tchotkeys.c app_res.o -lcomctl32