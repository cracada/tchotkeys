// tc-hotkeys.c
// 编译: gcc -O2 -s -mwindows -finput-charset=UTF-8 -fexec-charset=GBK -o tc-hotkeys.exe tc-hotkeys.c -lcomctl32
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__) && !defined(QueryFullProcessImageNameA)
BOOL WINAPI QueryFullProcessImageNameA(HANDLE hProcess, DWORD dwFlags, LPSTR lpExeName, PDWORD lpdwSize);
#endif

#define MAX_ROWS 8192
#define MAX_LEN  512
#define MAX_PARTS 64
#define MAX_DICT 4096
#define MAX_MENU_ITEMS 4096

typedef struct {
    char desc[MAX_LEN];
    char cmd[MAX_LEN];
    char hotkey[MAX_LEN];
    char code[MAX_LEN];
    int  srcLevel;
} Row;

typedef struct {
    char desc[MAX_LEN];
    char cmd[MAX_LEN];
    char hotkey[MAX_LEN];
    int tag; // 0=无标记, 1=[+], 2=[*]
} MenuItem;

typedef struct { char code[32]; char cmd[128]; } CmdCodeMap;
typedef struct { char code[32]; char desc[MAX_LEN]; } CodeDescMap;
typedef struct { char cmd[128]; char desc[MAX_LEN]; } EmCmdMap;

static Row  g_rows[MAX_ROWS];
static int  g_count = 0;
static HWND g_list, g_main, g_status;
static HWND g_hTc = NULL; 
static HFONT g_listFont = NULL;
static char g_filter[64] = {0};
static int  g_filterActive = 0;
static int  g_isSymbol = 0;
static int  g_executing = 0;
static int  g_allowExec = 0;
static int  g_sortCol = -1;
static int  g_sortAsc = 1;
static int  g_indices[MAX_ROWS];
static int  g_list_count = 0;

static char g_tcDir[MAX_PATH] = {0}; 
static char g_exeDir[MAX_PATH] = {0};

static CmdCodeMap g_cmdCodeMap[MAX_DICT]; static int g_cmdCodeCount = 0;
static CodeDescMap g_codeDescMap[MAX_DICT]; static int g_codeDescCount = 0;
static EmCmdMap g_emCmdMap[MAX_DICT]; static int g_emCmdCount = 0;

static char g_default_hotkeys[MAX_ROWS][MAX_LEN];
static int g_default_hotkey_count = 0;

/* ================= 排序比较函数 ================= */

static int compareIndices(const void* a, const void* b) {
    int i1 = *(const int*)a; int i2 = *(const int*)b; int r = 0;
    if (g_sortCol == 1) r = _stricmp(g_rows[i1].cmd, g_rows[i2].cmd);
    else if (g_sortCol == 2) r = _stricmp(g_rows[i1].hotkey, g_rows[i2].hotkey);
    else if (g_sortCol == 3) r = _stricmp(g_rows[i1].code, g_rows[i2].code);
    if (r == 0) r = i1 - i2;
    return g_sortAsc ? r : -r;
}

/* ================= 辅助函数 ================= */

static char* readFile(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    
    char* buf = (char*)malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, sz, f); buf[sz] = 0; fclose(f);

    char* result = buf;
    int isUtf8 = 0, isUtf16Le = 0, isUtf16Be = 0;

    if (sz >= 2 && (unsigned char)buf[0] == 0xFF && (unsigned char)buf[1] == 0xFE) isUtf16Le = 1;
    else if (sz >= 2 && (unsigned char)buf[0] == 0xFE && (unsigned char)buf[1] == 0xFF) isUtf16Be = 1;
    else if (sz >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) isUtf8 = 1;
    else {
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, buf, -1, NULL, 0) > 0) isUtf8 = 1;
    }

    if (isUtf16Le) {
        wchar_t* wbuf = (wchar_t*)(buf + 2);
        int wlen = (sz - 2) / 2;
        int newSz = WideCharToMultiByte(CP_ACP, 0, wbuf, wlen, NULL, 0, NULL, NULL);
        result = (char*)malloc(newSz + 1);
        WideCharToMultiByte(CP_ACP, 0, wbuf, wlen, result, newSz, NULL, NULL);
        result[newSz] = 0;
        free(buf);
    } else if (isUtf16Be) {
        wchar_t* wbuf = (wchar_t*)malloc(sz);
        for (int i = 2; i < sz; i += 2) wbuf[(i-2)/2] = (unsigned char)buf[i] << 8 | (unsigned char)buf[i+1];
        int wlen = (sz - 2) / 2;
        int newSz = WideCharToMultiByte(CP_ACP, 0, wbuf, wlen, NULL, 0, NULL, NULL);
        result = (char*)malloc(newSz + 1);
        WideCharToMultiByte(CP_ACP, 0, wbuf, wlen, result, newSz, NULL, NULL);
        result[newSz] = 0;
        free(wbuf); free(buf);
    } else if (isUtf8) {
        const char* utf8Ptr = buf;
        if (sz >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) utf8Ptr += 3;
        int newSz = MultiByteToWideChar(CP_UTF8, 0, utf8Ptr, -1, NULL, 0);
        if (newSz > 0) {
            wchar_t* wbuf = (wchar_t*)malloc(newSz * sizeof(wchar_t));
            MultiByteToWideChar(CP_UTF8, 0, utf8Ptr, -1, wbuf, newSz);
            int gbkSz = WideCharToMultiByte(CP_ACP, 0, wbuf, -1, NULL, 0, NULL, NULL);
            if (gbkSz > 0) {
                result = (char*)malloc(gbkSz + 1);
                WideCharToMultiByte(CP_ACP, 0, wbuf, -1, result, gbkSz, NULL, NULL);
                free(buf);
            }
            free(wbuf);
        }
    }
    return result;
}

static void trim(char* s) {
    if(!s || !*s) return;
    int n = (int)strlen(s);
    while (n > 0 && (s[n-1]==' '||s[n-1]=='\r'||s[n-1]=='\n'||s[n-1]=='\t')) s[--n] = 0;
    char* p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
}

static void replaceAll(char* s, const char* from, const char* to) {
    size_t fl = strlen(from), tl = strlen(to);
    if (fl == 0) return;
    char* p = s;
    while ((p = strstr(p, from)) != NULL) {
        memmove(p + tl, p + fl, strlen(p + fl) + 1);
        memcpy(p, to, tl);
        p += tl;
    }
}

static void normalizeHotkey(const char* raw, char* out) {
    char buf[MAX_LEN]; strncpy(buf, raw, MAX_LEN-1); buf[MAX_LEN-1]=0;
    trim(buf);
    
    replaceAll(buf, "OEM_US`~", "`");
    replaceAll(buf, "OEM_US[{", "[");
    replaceAll(buf, "OEM_US]}", "]");
    replaceAll(buf, "OEM_-", "-");
    replaceAll(buf, "OEM_+", "=");
    
    if(strstr(buf, "<") && (strstr(buf, "Ctrl") || strstr(buf, "CTRL") || strstr(buf, "ctrl"))) { 
        out[0] = 0; return; 
    }

    int c=0, s=0, a=0, w=0;
    char key[128] = {0};
    
    if (strchr(buf, '+') && !strchr(buf, ' ') && strlen(buf) < 32) {
        char* p = buf;
        while (*p && *p != '+') {
            if (*p == 'C' || *p == 'c') c = 1;
            else if (*p == 'S' || *p == 's') s = 1;
            else if (*p == 'A' || *p == 'a') a = 1;
            else if (*p == 'W' || *p == 'w') w = 1;
            p++;
        }
        if (*p == '+') strcpy(key, p+1);
    } else {
        char temp[MAX_LEN]; strcpy(temp, buf);
        char* p = temp;
        while (*p) {
            if (_strnicmp(p, "Ctrl", 4)==0) { c=1; p+=4; }
            else if (_strnicmp(p, "Shift", 5)==0) { s=1; p+=5; }
            else if (_strnicmp(p, "Alt", 3)==0) { a=1; p+=3; }
            else if (_strnicmp(p, "Win", 3)==0) { w=1; p+=3; }
            else break;
            while (*p == ' ' || *p == '+') p++;
        }
        strcpy(key, p);
    }
    
    char keyNoSpace[128] = {0};
    int j = 0;
    for(int i=0; key[i]; i++) {
        if (key[i] != ' ') {
            keyNoSpace[j] = key[i];
            if(keyNoSpace[j]>='a'&&keyNoSpace[j]<='z') keyNoSpace[j]-=32;
            j++;
        }
    }

    out[0] = 0;
    if (c) strcat(out, "CTRL+");
    if (a) strcat(out, "ALT+");
    if (s) strcat(out, "SHIFT+");
    if (w) strcat(out, "WIN+");
    strcat(out, keyNoSpace);
}

static void DetectTcDir() {
    g_tcDir[0] = 0;
    HWND hTc = FindWindowA("TTOTAL_CMD", NULL);
    
    // 1. 找运行中的 TC 进程
    if (hTc) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hTc, &pid);
        if (pid) {
            HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (hProc) {
                char tcExe[MAX_PATH] = {0};
                DWORD size = MAX_PATH;
                if (QueryFullProcessImageNameA(hProc, 0, tcExe, &size)) {
                    char* slash = strrchr(tcExe, '\\');
                    if (slash) {
                        *slash = 0;
                        strcpy(g_tcDir, tcExe);
                    }
                }
                CloseHandle(hProc);
            }
        }
    }
    
    // 2. 找环境变量
    if (!g_tcDir[0]) {
        if (GetEnvironmentVariableA("COMMANDER_PATH", g_tcDir, MAX_PATH) == 0) {
            g_tcDir[0] = 0;
        }
    }
    
    // 3. 向上查找特征文件（totalcmd.inc 或 wincmd.ini）
    if (!g_tcDir[0]) {
        char testPath[MAX_PATH];
        const char* rels[] = { "\\..", "\\..\\..", "\\..\\..\\.." };
        
        for (int i = 0; i < 3; i++) {
            snprintf(testPath, sizeof(testPath), "%s%s", g_exeDir, rels[i]);
            
            char check1[MAX_PATH], check2[MAX_PATH];
            snprintf(check1, sizeof(check1), "%s\\totalcmd.inc", testPath);
            snprintf(check2, sizeof(check2), "%s\\wincmd.ini", testPath);
            
            FILE* f1 = fopen(check1, "rb");
            if (f1) {
                fclose(f1);
                strcpy(g_tcDir, testPath);
                return;
            }
            FILE* f2 = fopen(check2, "rb");
            if (f2) {
                fclose(f2);
                strcpy(g_tcDir, testPath);
                return;
            }
        }
        
        // 4. 如果都没找到，强制退 3 级
        char exePath[MAX_PATH];
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        char* slash = strrchr(exePath, '\\');
        if (slash) *slash = 0; 
        for (int i = 0; i < 3; i++) {
            slash = strrchr(exePath, '\\');
            if (slash) *slash = 0;
        }
        strcpy(g_tcDir, exePath);
    }
}

static int findOrAddBinding(const char* rawHotkey, const char* cmd, int level) {
    char norm[MAX_LEN];
    normalizeHotkey(rawHotkey, norm);
    if (!norm[0]) return -1;

    if (level == 1 && g_default_hotkey_count < MAX_ROWS) {
        int found = 0;
        for (int i = 0; i < g_default_hotkey_count; i++) {
            if (strcmp(g_default_hotkeys[i], norm) == 0) { found = 1; break; }
        }
        if (!found) strcpy(g_default_hotkeys[g_default_hotkey_count++], norm);
    }

    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_rows[i].hotkey, norm) == 0) {
            if (level >= g_rows[i].srcLevel) {
                strcpy(g_rows[i].cmd, cmd);
                g_rows[i].srcLevel = level;
            }
            return i;
        }
    }
    if (g_count < MAX_ROWS) {
        strcpy(g_rows[g_count].hotkey, norm);
        strcpy(g_rows[g_count].cmd, cmd);
        g_rows[g_count].srcLevel = level;
        g_count++;
        return g_count - 1;
    }
    return -1;
}

/* ================= 加载字典 ================= */

static void loadTotalCmdInc(const char* path) {
    char* text = readFile(path);
    if(!text) return;
    char* line = strtok(text, "\n");
    while(line) {
        trim(line);
        if (line[0] != '[' && line[0] != ';' && strchr(line, '=')) {
            char* eq = strchr(line, '=');
            *eq = 0;
            char cmd[128], code[32];
            strncpy(cmd, line, sizeof(cmd)); trim(cmd);
            char* semi = strchr(eq+1, ';');
            if (semi) *semi = 0;
            strncpy(code, eq+1, sizeof(code)); trim(code);
            if (strncmp(cmd, "cm_", 3)==0 && g_cmdCodeCount < MAX_DICT) {
                strcpy(g_cmdCodeMap[g_cmdCodeCount].cmd, cmd);
                strcpy(g_cmdCodeMap[g_cmdCodeCount].code, code);
                g_cmdCodeCount++;
            }
        }
        line = strtok(NULL, "\n");
    }
    free(text);
}

static void loadWcmdChnInc(const char* path) {
    char* text = readFile(path);
    if(!text) return;
    char* line = strtok(text, "\n");
    while(line) {
        trim(line);
        char* eq = strchr(line, '=');
        if (eq) {
            *eq = 0;
            char code[32], desc[MAX_LEN];
            strncpy(code, line, sizeof(code)); trim(code);
            strncpy(desc, eq+1, sizeof(desc)); trim(desc);
            int dl = strlen(desc);
            if (dl >= 2 && desc[0]=='"' && desc[dl-1]=='"') {
                desc[dl-1]=0; memmove(desc, desc+1, dl);
            }
            if (g_codeDescCount < MAX_DICT) {
                strcpy(g_codeDescMap[g_codeDescCount].code, code);
                strcpy(g_codeDescMap[g_codeDescCount].desc, desc);
                g_codeDescCount++;
            }
        }
        line = strtok(NULL, "\n");
    }
    free(text);
}

static void loadUserCmdIni(const char* path1, const char* path2) {
    char* text = readFile(path1);
    if(!text) text = readFile(path2);
    if(!text) return;
    
    char curCmd[128] = {0}, curDesc[MAX_LEN] = {0};
    char* line = strtok(text, "\n");
    while(line) {
        trim(line);
        if (line[0] == '[') {
            if (curCmd[0] && g_emCmdCount < MAX_DICT) {
                strcpy(g_emCmdMap[g_emCmdCount].cmd, curCmd);
                strcpy(g_emCmdMap[g_emCmdCount].desc, curDesc[0] ? curDesc : curCmd);
                g_emCmdCount++;
            }
            strncpy(curCmd, line+1, sizeof(curCmd)); trim(curCmd);
            char* br = strchr(curCmd, ']'); if(br) *br = 0;
            curDesc[0] = 0;
        } else if (strncmp(line, "menu=", 5) == 0) {
            strncpy(curDesc, line+5, sizeof(curDesc)); trim(curDesc);
            int dl = strlen(curDesc);
            if (dl >= 2 && curDesc[0] == '"' && curDesc[dl-1] == '"') {
                curDesc[dl-1] = 0; memmove(curDesc, curDesc + 1, dl);
            }
        }
        line = strtok(NULL, "\n");
    }
    if (curCmd[0] && g_emCmdCount < MAX_DICT) {
        strcpy(g_emCmdMap[g_emCmdCount].cmd, curCmd);
        strcpy(g_emCmdMap[g_emCmdCount].desc, curDesc[0] ? curDesc : curCmd);
        g_emCmdCount++;
    }
    free(text);
}

static void loadHotkeysTxt(const char* path) {
    char* text = readFile(path);
    if(!text) return;
    char* line = strtok(text, "\n");
    while(line) {
        trim(line);
        if (*line && line[0] != '#' && !strstr(line, "=====") && strcmp(line, "描述\t命令\t默认热键") != 0) {
            char cmd[128] = {0};
            char keys[MAX_LEN] = {0};
            
            if (strchr(line, '\t')) {
                char* p1 = strchr(line, '\t');
                if (p1) {
                    *p1 = 0; p1++;
                    char* p2 = strchr(p1, '\t');
                    if (p2) {
                        *p2 = 0; p2++;
                        strcpy(cmd, p1); trim(cmd);
                        strcpy(keys, p2);
                    }
                }
            } else if (strchr(line, '=')) {
                char* eq = strchr(line, '=');
                *eq = 0;
                char code[32]; strcpy(code, line); trim(code);
                strcpy(keys, eq+1);
                for (int i=0; i<g_cmdCodeCount; i++) {
                    if (strcmp(g_cmdCodeMap[i].code, code) == 0) {
                        strcpy(cmd, g_cmdCodeMap[i].cmd); break;
                    }
                }
                if (!cmd[0] && code[0]) {
                    snprintf(cmd, sizeof(cmd), "cm_%s", code);
                }
            }

            if (cmd[0] && keys[0]) {
                char* keyStart = keys;
                char* eq = strstr(keyStart, " = ");
                while (keyStart && *keyStart) {
                    char singleKey[MAX_LEN];
                    if (eq) {
                        *eq = 0; strcpy(singleKey, keyStart); keyStart = eq + 3; eq = strstr(keyStart, " = ");
                    } else {
                        strcpy(singleKey, keyStart); keyStart = NULL;
                    }
                    findOrAddBinding(singleKey, cmd, 1);
                }
            }
        }
        line = strtok(NULL, "\n");
    }
    free(text);
}

static void loadIniShortcuts(const char* path, int level, int needSection) {
    static int depth = 0;
    if (depth > 5) return; 
    depth++;

    char* text = readFile(path);
    if(!text) { depth--; return; }
    
    int inShortcuts = needSection ? 0 : 1;
    char* line = strtok(text, "\n");
    while(line) {
        trim(line);
        if (line[0] == '[') {
            if (_stricmp(line, "[Shortcuts]") == 0) inShortcuts = 1;
            else if (needSection) inShortcuts = 0;
        } else if (inShortcuts && line[0] != ';' && line[0] != '#' && strchr(line, '=')) {
            char* eq = strchr(line, '=');
            *eq = 0;
            char hotkey[MAX_LEN], cmd[MAX_LEN];
            strcpy(hotkey, line); trim(hotkey);
            strcpy(cmd, eq+1); trim(cmd);
            if (hotkey[0] && cmd[0]) {
                if (_stricmp(hotkey, "RedirectSection") == 0) {
                    char redirectPath[MAX_PATH];
                    if (strstr(cmd, "%COMMANDER_PATH%")) {
                        snprintf(redirectPath, MAX_PATH, "%s%s", g_tcDir, cmd + 16); 
                    } else if (cmd[0] == '\\' || (cmd[1] == ':' && cmd[2] == '\\')) {
                        strncpy(redirectPath, cmd, MAX_PATH);
                    } else {
                        snprintf(redirectPath, MAX_PATH, "%s\\%s", g_tcDir, cmd);
                    }
                    
                    FILE* tf = fopen(redirectPath, "rb");
                    if (!tf) {
                        if (cmd[0] != '\\' && cmd[1] != ':') {
                            snprintf(redirectPath, MAX_PATH, "%s\\User\\%s", g_tcDir, cmd);
                        }
                    } else {
                        fclose(tf);
                    }
                    
                    loadIniShortcuts(redirectPath, level, 0);
                } else {
                    findOrAddBinding(hotkey, cmd, level);
                }
            }
        }
        line = strtok(NULL, "\n");
    }
    free(text);
    depth--;
}

static void buildFinalRows() {
    for (int i = 0; i < g_count; i++) {
        char finalDesc[MAX_LEN] = {0};
        char finalCode[MAX_LEN] = {0};
        char* cmd = g_rows[i].cmd;

        for (int j = 0; j < g_cmdCodeCount; j++) {
            if (strcmp(g_cmdCodeMap[j].cmd, cmd) == 0) {
                strcpy(finalCode, g_cmdCodeMap[j].code);
                break;
            }
        }

        if (strncmp(cmd, "em_", 3) == 0) {
            for (int j = 0; j < g_emCmdCount; j++) {
                if (_stricmp(g_emCmdMap[j].cmd, cmd) == 0) {
                    strcpy(finalDesc, g_emCmdMap[j].desc);
                    break;
                }
            }
            if (!finalDesc[0]) strcpy(finalDesc, cmd);
        } else {
            if (finalCode[0]) {
                for (int j = 0; j < g_codeDescCount; j++) {
                    if (strcmp(g_codeDescMap[j].code, finalCode) == 0) {
                        strcpy(finalDesc, g_codeDescMap[j].desc);
                        break;
                    }
                }
            }
            if (!finalDesc[0]) strcpy(finalDesc, cmd);
        }

        strcpy(g_rows[i].desc, finalDesc);
        strcpy(g_rows[i].code, finalCode);
    }
}

static void loadAllData(const char* exeDir) {
    g_count = 0;
    g_default_hotkey_count = 0;
    
    char path_totalcmd[MAX_PATH], path_wcmd[MAX_PATH], path_usercmd1[MAX_PATH], path_usercmd2[MAX_PATH], path_wincmd[MAX_PATH];
    
    snprintf(path_totalcmd, MAX_PATH, "%s\\totalcmd.inc", g_tcDir);
    snprintf(path_wcmd, MAX_PATH, "%s\\language\\wcmd_chn.inc", g_tcDir);
    snprintf(path_usercmd1, MAX_PATH, "%s\\User\\usercmd.ini", g_tcDir);
    snprintf(path_usercmd2, MAX_PATH, "%s\\usercmd.ini", g_tcDir);
    snprintf(path_wincmd, MAX_PATH, "%s\\wincmd.ini", g_tcDir);
    
    loadTotalCmdInc(path_totalcmd);
    loadWcmdChnInc(path_wcmd);
    loadUserCmdIni(path_usercmd1, path_usercmd2);

    // 自定义快捷键文件严格只读取 exe 同目录
    char path_hotkeys[MAX_PATH];
    char path_zdykeys[MAX_PATH];
    
    snprintf(path_hotkeys, MAX_PATH, "%s\\tc_hotkeys.txt", exeDir);
    snprintf(path_zdykeys, MAX_PATH, "%s\\tc_zdykeys.txt", exeDir);

    loadHotkeysTxt(path_hotkeys); 
    loadIniShortcuts(path_wincmd, 2, 1);  
    loadIniShortcuts(path_zdykeys, 3, 0); 

    buildFinalRows();
}

/* ================= 菜单导出逻辑 ================= */

static MenuItem list_F[MAX_MENU_ITEMS]; static int cnt_F = 0;
static MenuItem list_AltF[MAX_MENU_ITEMS]; static int cnt_AltF = 0;
static MenuItem list_ShiftF[MAX_MENU_ITEMS]; static int cnt_ShiftF = 0;
static MenuItem list_CtrlF[MAX_MENU_ITEMS]; static int cnt_CtrlF = 0;

static MenuItem list_AltNum[MAX_MENU_ITEMS]; static int cnt_AltNum = 0;
static MenuItem list_ShiftNum[MAX_MENU_ITEMS]; static int cnt_ShiftNum = 0;
static MenuItem list_CtrlNum[MAX_MENU_ITEMS]; static int cnt_CtrlNum = 0;

static MenuItem list_AltAlpha[MAX_MENU_ITEMS]; static int cnt_AltAlpha = 0;
static MenuItem list_ShiftAlpha[MAX_MENU_ITEMS]; static int cnt_ShiftAlpha = 0;
static MenuItem list_CtrlAlpha[MAX_MENU_ITEMS]; static int cnt_CtrlAlpha = 0;

static MenuItem list_AltOther[MAX_MENU_ITEMS]; static int cnt_AltOther = 0;
static MenuItem list_ShiftOther[MAX_MENU_ITEMS]; static int cnt_ShiftOther = 0;
static MenuItem list_CtrlOther[MAX_MENU_ITEMS]; static int cnt_CtrlOther = 0;

static MenuItem list_Three_CA[MAX_MENU_ITEMS]; static int cnt_Three_CA = 0;
static MenuItem list_Three_CS[MAX_MENU_ITEMS]; static int cnt_Three_CS = 0;
static MenuItem list_Three_AS[MAX_MENU_ITEMS]; static int cnt_Three_AS = 0;
static MenuItem list_Three_Other[MAX_MENU_ITEMS]; static int cnt_Three_Other = 0;

static MenuItem list_NoCmd[MAX_MENU_ITEMS]; static int cnt_NoCmd = 0;
static MenuItem list_Other[MAX_MENU_ITEMS]; static int cnt_Other = 0;

void ParseHotkeyStr(const char* hk, int* ctrl, int* alt, int* shift, int* win, char* mainKey) {
    *ctrl = *alt = *shift = *win = 0;
    mainKey[0] = 0;
    const char* p = hk;
    while (*p) {
        if (strncmp(p, "CTRL+", 5) == 0) { *ctrl = 1; p += 5; }
        else if (strncmp(p, "SHIFT+", 6) == 0) { *shift = 1; p += 6; }
        else if (strncmp(p, "ALT+", 4) == 0) { *alt = 1; p += 4; }
        else if (strncmp(p, "WIN+", 4) == 0) { *win = 1; p += 4; }
        else break;
    }
    strcpy(mainKey, p);
    if (mainKey[0] == 0 && strchr(hk, '+')) strcpy(mainKey, "+");
}

int GetKeySortWeight(const char* mainKey) {
    if (strlen(mainKey) == 1) {
        char c = mainKey[0];
        if (c == '0') return 10;
        if (c >= '1' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'Z') return c - 'A' + 20;
    }
    if (strcmp(mainKey, "LEFT") == 0) return 50;
    if (strcmp(mainKey, "RIGHT") == 0) return 51;
    if (strcmp(mainKey, "UP") == 0) return 52;
    if (strcmp(mainKey, "DOWN") == 0) return 53;
    if (strcmp(mainKey, "TAB") == 0) return 54;
    if (strcmp(mainKey, "ESC") == 0) return 55;
    if (strcmp(mainKey, "SPACE") == 0) return 56;
    if (strcmp(mainKey, "ENTER") == 0) return 57;
    if (strcmp(mainKey, "BACK") == 0) return 58;
    return 100 + (unsigned char)mainKey[0];
}

int cmp_F(const void* a, const void* b) {
    MenuItem* m1 = (MenuItem*)a;
    MenuItem* m2 = (MenuItem*)b;
    char k1[128], k2[128]; int c,a1,s,w;
    ParseHotkeyStr(m1->hotkey, &c, &a1, &s, &w, k1);
    ParseHotkeyStr(m2->hotkey, &c, &a1, &s, &w, k2);
    int f1 = atoi(k1 + 1);
    int f2 = atoi(k2 + 1);
    return f1 - f2;
}

int cmp_Key(const void* a, const void* b) {
    MenuItem* m1 = (MenuItem*)a;
    MenuItem* m2 = (MenuItem*)b;
    char k1[128], k2[128]; int c,a1,s,w;
    ParseHotkeyStr(m1->hotkey, &c, &a1, &s, &w, k1);
    ParseHotkeyStr(m2->hotkey, &c, &a1, &s, &w, k2);
    int w1 = GetKeySortWeight(k1);
    int w2 = GetKeySortWeight(k2);
    if (w1 != w2) return w1 - w2;
    return strcmp(m1->hotkey, m2->hotkey);
}

void ProcessMenuItem(MenuItem* item) {
    char mainKey[128] = {0};
    int ctrl = 0, alt = 0, shift = 0, win = 0;
    ParseHotkeyStr(item->hotkey, &ctrl, &alt, &shift, &win, mainKey);
    
    int isFunc = 0, isNum = 0, isAlpha = 0;
    if (strlen(mainKey) >= 2 && mainKey[0] == 'F') {
        int fNum = atoi(mainKey + 1);
        if (fNum >= 1 && fNum <= 24) isFunc = 1;
    }
    if (strlen(mainKey) == 1) {
        if (mainKey[0] >= '0' && mainKey[0] <= '9') isNum = 1;
        else if (mainKey[0] >= 'A' && mainKey[0] <= 'Z') isAlpha = 1;
    }
    
    int noCmd = (strncmp(item->cmd, "cm_", 3) != 0 && strncmp(item->cmd, "em_", 3) != 0);
    int modCount = ctrl + alt + shift + win;
    
    if (noCmd) { list_NoCmd[cnt_NoCmd++] = *item; return; }
    
    if (modCount >= 2) {
        if (ctrl && alt) {
            list_Three_CA[cnt_Three_CA++] = *item;
        } else if (ctrl && shift) {
            list_Three_CS[cnt_Three_CS++] = *item;
        } else if (alt && shift) {
            list_Three_AS[cnt_Three_AS++] = *item;
        } else {
            list_Three_Other[cnt_Three_Other++] = *item;
        }
        return;
    }
    
    if (modCount == 1) {
        if (isFunc) {
            if (alt) list_AltF[cnt_AltF++] = *item;
            else if (shift) list_ShiftF[cnt_ShiftF++] = *item;
            else if (ctrl) list_CtrlF[cnt_CtrlF++] = *item;
            else list_Other[cnt_Other++] = *item;
        } else if (isNum) {
            if (alt) list_AltNum[cnt_AltNum++] = *item;
            else if (shift) list_ShiftNum[cnt_ShiftNum++] = *item;
            else if (ctrl) list_CtrlNum[cnt_CtrlNum++] = *item;
            else list_Other[cnt_Other++] = *item;
        } else if (isAlpha) {
            if (alt) list_AltAlpha[cnt_AltAlpha++] = *item;
            else if (shift) list_ShiftAlpha[cnt_ShiftAlpha++] = *item;
            else if (ctrl) list_CtrlAlpha[cnt_CtrlAlpha++] = *item;
            else list_Other[cnt_Other++] = *item;
        } else {
            if (alt) list_AltOther[cnt_AltOther++] = *item;
            else if (shift) list_ShiftOther[cnt_ShiftOther++] = *item;
            else if (ctrl) list_CtrlOther[cnt_CtrlOther++] = *item;
            else list_Other[cnt_Other++] = *item;
        }
        return;
    }
    if (modCount == 0) {
        if (isFunc) list_F[cnt_F++] = *item;
        else list_Other[cnt_Other++] = *item;
    }
}

void FormatHotkeyDisplay(const char* hk, char* out) {
    strcpy(out, hk);
    replaceAll(out, "CTRL+", "Ctrl + ");
    replaceAll(out, "ALT+", "Alt + ");
    replaceAll(out, "SHIFT+", "Shift + ");
    replaceAll(out, "WIN+", "Win + ");
    replaceAll(out, "LEFT", "←");
    replaceAll(out, "RIGHT", "→");
    replaceAll(out, "UP", "↑");
    replaceAll(out, "DOWN", "↓");
}

void WriteItems(FILE* f, MenuItem* list, int count) {
    for (int i = 0; i < count; i++) {
        char hkDisp[MAX_LEN];
        FormatHotkeyDisplay(list[i].hotkey, hkDisp);
        
        fprintf(f, "    MENUITEM \"%s\\t%s\", %s\n", list[i].desc, hkDisp, list[i].cmd);
    }
}

int ExportMnu(const char* exeDir) {
    char path[MAX_PATH];
    snprintf(path, MAX_PATH, "%s\\output_hotkeys.mnu", exeDir);
    FILE* f = fopen(path, "w");
    if (!f) return 0;
    
    int total = 0;
    for (int i = 0; i < g_count; i++) {
        if (g_rows[i].hotkey[0] == 0) continue;
        total++;
        MenuItem item;
        strcpy(item.desc, g_rows[i].desc);
        strcpy(item.cmd, g_rows[i].cmd);
        strcpy(item.hotkey, g_rows[i].hotkey);
        
        ProcessMenuItem(&item);
    }
    
    if (cnt_F > 1) qsort(list_F, cnt_F, sizeof(MenuItem), cmp_F);
    if (cnt_AltF > 1) qsort(list_AltF, cnt_AltF, sizeof(MenuItem), cmp_F);
    if (cnt_ShiftF > 1) qsort(list_ShiftF, cnt_ShiftF, sizeof(MenuItem), cmp_F);
    if (cnt_CtrlF > 1) qsort(list_CtrlF, cnt_CtrlF, sizeof(MenuItem), cmp_F);
    
    if (cnt_AltNum > 1) qsort(list_AltNum, cnt_AltNum, sizeof(MenuItem), cmp_Key);
    if (cnt_ShiftNum > 1) qsort(list_ShiftNum, cnt_ShiftNum, sizeof(MenuItem), cmp_Key);
    if (cnt_CtrlNum > 1) qsort(list_CtrlNum, cnt_CtrlNum, sizeof(MenuItem), cmp_Key);
    
    if (cnt_AltAlpha > 1) qsort(list_AltAlpha, cnt_AltAlpha, sizeof(MenuItem), cmp_Key);
    if (cnt_ShiftAlpha > 1) qsort(list_ShiftAlpha, cnt_ShiftAlpha, sizeof(MenuItem), cmp_Key);
    if (cnt_CtrlAlpha > 1) qsort(list_CtrlAlpha, cnt_CtrlAlpha, sizeof(MenuItem), cmp_Key);
    
    if (cnt_AltOther > 1) qsort(list_AltOther, cnt_AltOther, sizeof(MenuItem), cmp_Key);
    if (cnt_ShiftOther > 1) qsort(list_ShiftOther, cnt_ShiftOther, sizeof(MenuItem), cmp_Key);
    if (cnt_CtrlOther > 1) qsort(list_CtrlOther, cnt_CtrlOther, sizeof(MenuItem), cmp_Key);

    if (cnt_Three_CA > 1) qsort(list_Three_CA, cnt_Three_CA, sizeof(MenuItem), cmp_Key);
    if (cnt_Three_CS > 1) qsort(list_Three_CS, cnt_Three_CS, sizeof(MenuItem), cmp_Key);
    if (cnt_Three_AS > 1) qsort(list_Three_AS, cnt_Three_AS, sizeof(MenuItem), cmp_Key);
    if (cnt_Three_Other > 1) qsort(list_Three_Other, cnt_Three_Other, sizeof(MenuItem), cmp_Key);

    if (cnt_NoCmd > 1) qsort(list_NoCmd, cnt_NoCmd, sizeof(MenuItem), cmp_Key);
    if (cnt_Other > 1) qsort(list_Other, cnt_Other, sizeof(MenuItem), cmp_Key);

    fprintf(f, "POPUP \"快捷键(&K)\"\n");
    fprintf(f, "    MENUITEM \"快捷键查询\\tCtrl + Shift + E\", em_HotkeysQuery\n");
    fprintf(f, "    MENUITEM SEPARATOR\n");

    if (cnt_F > 0) {
        fprintf(f, "    POPUP \"功能键\\tFx\"\n");
        WriteItems(f, list_F, cnt_F);
        fprintf(f, "    END_POPUP\n");
    }

    if (cnt_AltF > 0 || cnt_ShiftF > 0 || cnt_CtrlF > 0) {
        if (cnt_AltF > 0) { fprintf(f, "    POPUP \"Alt +\\tFx\"\n"); WriteItems(f, list_AltF, cnt_AltF); fprintf(f, "    END_POPUP\n"); }
        if (cnt_ShiftF > 0) { fprintf(f, "    POPUP \"Shift +\\tFx\"\n"); WriteItems(f, list_ShiftF, cnt_ShiftF); fprintf(f, "    END_POPUP\n"); }
        if (cnt_CtrlF > 0) { fprintf(f, "    POPUP \"Ctrl +\\tFx\"\n"); WriteItems(f, list_CtrlF, cnt_CtrlF); fprintf(f, "    END_POPUP\n"); }
        fprintf(f, "\n    MENUITEM SEPARATOR\n\n");
    }

    if (cnt_AltNum > 0 || cnt_ShiftNum > 0 || cnt_CtrlNum > 0) {
        if (cnt_AltNum > 0) { fprintf(f, "    POPUP \"Alt +\\t数字\"\n"); WriteItems(f, list_AltNum, cnt_AltNum); fprintf(f, "    END_POPUP\n"); }
        if (cnt_CtrlNum > 0) { fprintf(f, "    POPUP \"Ctrl +\\t数字\"\n"); WriteItems(f, list_CtrlNum, cnt_CtrlNum); fprintf(f, "    END_POPUP\n"); }
        if (cnt_ShiftNum > 0) { fprintf(f, "    POPUP \"Shift +\\t数字\"\n"); WriteItems(f, list_ShiftNum, cnt_ShiftNum); fprintf(f, "    END_POPUP\n"); }
        fprintf(f, "\n    MENUITEM SEPARATOR\n\n");
    }

    if (cnt_AltAlpha > 0 || cnt_ShiftAlpha > 0 || cnt_CtrlAlpha > 0) {
        if (cnt_AltAlpha > 0) { fprintf(f, "    POPUP \"Alt +\\t字母\"\n"); WriteItems(f, list_AltAlpha, cnt_AltAlpha); fprintf(f, "    END_POPUP\n"); }
        if (cnt_ShiftAlpha > 0) { fprintf(f, "    POPUP \"Shift +\\t字母\"\n"); WriteItems(f, list_ShiftAlpha, cnt_ShiftAlpha); fprintf(f, "    END_POPUP\n"); }
        if (cnt_CtrlAlpha > 0) { fprintf(f, "    POPUP \"Ctrl +\\t字母\"\n"); WriteItems(f, list_CtrlAlpha, cnt_CtrlAlpha); fprintf(f, "    END_POPUP\n"); }
        fprintf(f, "\n    MENUITEM SEPARATOR\n\n");
    }

    if (cnt_AltOther > 0 || cnt_ShiftOther > 0 || cnt_CtrlOther > 0) {
        if (cnt_AltOther > 0) { fprintf(f, "    POPUP \"Alt +\\t其他\"\n"); WriteItems(f, list_AltOther, cnt_AltOther); fprintf(f, "    END_POPUP\n"); }
        if (cnt_ShiftOther > 0) { fprintf(f, "    POPUP \"Shift +\\t其他\"\n"); WriteItems(f, list_ShiftOther, cnt_ShiftOther); fprintf(f, "    END_POPUP\n"); }
        if (cnt_CtrlOther > 0) { fprintf(f, "    POPUP \"Ctrl +\\t其他\"\n"); WriteItems(f, list_CtrlOther, cnt_CtrlOther); fprintf(f, "    END_POPUP\n"); }
        fprintf(f, "\n    MENUITEM SEPARATOR\n\n");
    }

    if (cnt_Three_CA > 0 || cnt_Three_CS > 0 || cnt_Three_AS > 0 || cnt_Three_Other > 0) {
        if (cnt_Three_CA > 0) { fprintf(f, "    POPUP \"三键 (Ctrl + Alt + ...)\"\n"); WriteItems(f, list_Three_CA, cnt_Three_CA); fprintf(f, "    END_POPUP\n"); }
        if (cnt_Three_CS > 0) { fprintf(f, "    POPUP \"三键 (Ctrl + Shift + ...)\"\n"); WriteItems(f, list_Three_CS, cnt_Three_CS); fprintf(f, "    END_POPUP\n"); }
        if (cnt_Three_AS > 0) { fprintf(f, "    POPUP \"三键 (Alt + Shift + ...)\"\n"); WriteItems(f, list_Three_AS, cnt_Three_AS); fprintf(f, "    END_POPUP\n"); }
        if (cnt_Three_Other > 0) { fprintf(f, "    POPUP \"三键 (其它组合)\"\n"); WriteItems(f, list_Three_Other, cnt_Three_Other); fprintf(f, "    END_POPUP\n"); }
    }

    if (cnt_NoCmd > 0) {
        fprintf(f, "    POPUP \"有键无命令\"\n");
        WriteItems(f, list_NoCmd, cnt_NoCmd);
        fprintf(f, "    END_POPUP\n");
    }

    if (cnt_Other > 0) {
        fprintf(f, "\n    MENUITEM SEPARATOR\n\n");
        fprintf(f, "    POPUP \"其他\"\n");
        WriteItems(f, list_Other, cnt_Other);
        fprintf(f, "    END_POPUP\n");
    }

    fprintf(f, "END_POPUP\n");
    fclose(f);
    return total;
}

/* ================= TXT 导出逻辑 ================= */

int ExportTxt(const char* exeDir) {
    char path[MAX_PATH];
    snprintf(path, MAX_PATH, "%s\\output_hotkeys.txt", exeDir);
    FILE* f = fopen(path, "w");
    if (!f) return 0;

    int oldSortCol = g_sortCol;
    int oldSortAsc = g_sortAsc;
    g_sortCol = 2;
    g_sortAsc = 1;

    int* indices = (int*)malloc(g_count * sizeof(int));
    for (int i = 0; i < g_count; i++) indices[i] = i;
    qsort(indices, g_count, sizeof(int), compareIndices);

    int total = 0;
    fprintf(f, "描述\t命令\t热键\t代码\t标记\n");
    for (int k = 0; k < g_count; k++) {
        int i = indices[k];
        if (g_rows[i].hotkey[0] == 0) continue;
        total++;

        char tag[16] = "";
        if (g_rows[i].srcLevel > 1) {
            int found = 0;
            for (int j = 0; j < g_default_hotkey_count; j++) {
                if (strcmp(g_rows[i].hotkey, g_default_hotkeys[j]) == 0) { found = 1; break; }
            }
            strcpy(tag, found ? "[*]" : "[+]");
        }
        fprintf(f, "%s\t%s\t%s\t%s\t%s\n", g_rows[i].desc, g_rows[i].cmd, g_rows[i].hotkey, g_rows[i].code, tag);
    }
    free(indices);
    fclose(f);

    g_sortCol = oldSortCol;
    g_sortAsc = oldSortAsc;
    return total;
}

/* ================= 菜单替换逻辑 ================= */

static int FindAllFinalHotkeys(const char* cmd, char* outHotkey) {
    outHotkey[0] = 0;
    int found = 0;
    
    for (int i = 0; i < g_count; i++) {
        if (_stricmp(g_rows[i].cmd, cmd) == 0) {
            if (outHotkey[0]) strcat(outHotkey, " / ");
            
            char hkDisp[MAX_LEN];
            FormatHotkeyDisplay(g_rows[i].hotkey, hkDisp);
            strcat(outHotkey, hkDisp);
            found = 1;
        }
    }
    return found;
}

int PatchMnu(const char* exeDir) {
    char inPath[MAX_PATH], outPath[MAX_PATH];
    snprintf(inPath, MAX_PATH, "%s\\input.mnu", exeDir);
    snprintf(outPath, MAX_PATH, "%s\\output_patched.mnu", exeDir);
    
    char* text = readFile(inPath);
    if (!text) return -1; 

    FILE* f = fopen(outPath, "w");
    if (!f) { free(text); return 0; }

    int total = 0;
    char* line = strtok(text, "\n");
    while (line) {
        char originalLine[MAX_LEN];
        strncpy(originalLine, line, MAX_LEN - 1);
        originalLine[MAX_LEN - 1] = 0;

        trim(line);
        if (strncmp(line, "MENUITEM", 8) == 0) {
            char* quote1 = strchr(line, '"');
            char* quote2 = strrchr(line, '"');
            
            if (quote1 && quote2 && quote2 > quote1) {
                char* pCmd = quote2 + 1;
                while (*pCmd == ',' || *pCmd == ' ' || *pCmd == '\t') pCmd++;
                
                char cmd[MAX_LEN] = {0};
                char* pSpace = pCmd;
                while (*pSpace && *pSpace != ',' && *pSpace != ' ' && *pSpace != '\t') pSpace++;
                int cmdLen = pSpace - pCmd;
                if (cmdLen > 0 && cmdLen < MAX_LEN) {
                    strncpy(cmd, pCmd, cmdLen);
                    cmd[cmdLen] = 0;
                }

                *quote2 = 0;
                char desc[MAX_LEN] = {0};
                strncpy(desc, quote1 + 1, MAX_LEN - 1);

                char finalHotkey[MAX_LEN] = {0};
                
                if (FindAllFinalHotkeys(cmd, finalHotkey)) {
                    if (finalHotkey[0]) {
                        char* oldTab = strstr(desc, "\\t");
                        if (oldTab) *oldTab = 0;
                        trim(desc);
                        
                        fprintf(f, "MENUITEM \"%s\\t%s\", %s\n", desc, finalHotkey, pCmd);
                        total++;
                    } else {
                        char* oldTab = strstr(desc, "\\t");
                        if (oldTab) *oldTab = 0;
                        trim(desc);
                        fprintf(f, "MENUITEM \"%s\", %s\n", desc, pCmd);
                    }
                } else {
                    fprintf(f, "%s\n", originalLine);
                }
                
                line = strtok(NULL, "\n");
                continue;
            }
        }
        fprintf(f, "%s\n", originalLine);
        line = strtok(NULL, "\n");
    }
    
    free(text);
    fclose(f);
    return total;
}

/* ================= 界面与搜索 ================= */

static int iequals(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static int icontains(const char* hay, const char* needle) {
    if (!needle || !*needle) return 1;
    for (const char* h = hay; *h; h++) {
        const char* a = h; const char* b = needle;
        while (*a && *b) {
            char ca = *a, cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca += 32;
            if (cb >= 'A' && cb <= 'Z') cb += 32;
            if (ca != cb) break;
            a++; b++;
        }
        if (!*b) return 1;
    }
    return 0;
}

static const char* lastKeyPart(const char* s) {
    const char* last = strrchr(s, '+');
    return last ? last + 1 : s;
}

static int matchHotkey(const char* hk, const char* key, int isSymbol) {
    if (!key || !*key) return 1;
    
    if (strncmp(key, "NUM", 3) == 0) {
        size_t hkLen = strlen(hk);
        size_t kLen = strlen(key);
        if (hkLen >= kLen && _stricmp(hk + hkLen - kLen, key) == 0) {
            if (hkLen == kLen || hk[hkLen - kLen - 1] == '+') return 1;
        }
        return 0;
    }
    
    if (isSymbol) {
        const char* last = lastKeyPart(hk);
        if (last && !*last) return icontains("+", key);
        if (strncmp(last, "NUM", 3) == 0) return 0;
        return icontains(last, key);
    }
    
    return iequals(lastKeyPart(hk), key);
}

static int vkToKeyStr(WPARAM vk, char* out, int outSize) {
    out[0] = 0;
    if (vk >= VK_F1 && vk <= VK_F12) { snprintf(out, outSize, "F%d", (int)(vk - VK_F1 + 1)); return 1; }
    if (vk >= 'A' && vk <= 'Z') { out[0] = (char)vk; out[1] = 0; return 1; }
    if (vk >= '0' && vk <= '9') { out[0] = (char)vk; out[1] = 0; return 1; }
    
    switch (vk) {
    case VK_BACK:     strcpy(out, "Back");  return 1;
    case VK_LEFT:     strcpy(out, "Left");  return 1;
    case VK_RIGHT:    strcpy(out, "Right"); return 1;
    case VK_UP:       strcpy(out, "Up");    return 1;
    case VK_DOWN:     strcpy(out, "Down");  return 1;
    case VK_HOME:     strcpy(out, "Home");  return 1;
    case VK_END:      strcpy(out, "End");   return 1;
    case VK_PRIOR:    strcpy(out, "PgUp");  return 1;
    case VK_NEXT:     strcpy(out, "PgDn");  return 1;
    case VK_INSERT:   strcpy(out, "Ins");   return 1;
    case VK_DELETE:   strcpy(out, "Del");   return 1;
    case VK_TAB:      strcpy(out, "Tab");   return 1;
    case VK_RETURN:   strcpy(out, "Enter"); return 1;
    case VK_SPACE:    strcpy(out, "Space"); return 1;
    
    case VK_ADD:      strcpy(out, "NUM+");  return -1;
    case VK_SUBTRACT: strcpy(out, "NUM-");  return -1;
    case VK_MULTIPLY: strcpy(out, "NUM*");  return -1;
    case VK_DIVIDE:   strcpy(out, "NUM/");  return -1;
    case VK_NUMPAD0:  strcpy(out, "NUM0");  return 1;
    case VK_NUMPAD1:  strcpy(out, "NUM1");  return 1;
    case VK_NUMPAD2:  strcpy(out, "NUM2");  return 1;
    case VK_NUMPAD3:  strcpy(out, "NUM3");  return 1;
    case VK_NUMPAD4:  strcpy(out, "NUM4");  return 1;
    case VK_NUMPAD5:  strcpy(out, "NUM5");  return 1;
    case VK_NUMPAD6:  strcpy(out, "NUM6");  return 1;
    case VK_NUMPAD7:  strcpy(out, "NUM7");  return 1;
    case VK_NUMPAD8:  strcpy(out, "NUM8");  return 1;
    case VK_NUMPAD9:  strcpy(out, "NUM9");  return 1;

    case VK_OEM_PLUS: strcpy(out, "=");     return -1;
    case VK_OEM_MINUS:strcpy(out, "-");     return -1;
    case VK_OEM_1:    strcpy(out, ";");     return -1;
    case VK_OEM_2:    strcpy(out, "/");     return -1;
    case VK_OEM_3:    strcpy(out, "`");     return -1;
    case VK_OEM_4:    strcpy(out, "[");     return -1;
    case VK_OEM_5:    strcpy(out, "\\");    return -1;
    case VK_OEM_6:    strcpy(out, "]");     return -1;
    case VK_OEM_7:    strcpy(out, "'");     return -1;
    case VK_OEM_COMMA:  strcpy(out, ",");   return -1;
    case VK_OEM_PERIOD: strcpy(out, ".");   return -1;
    }
    return 0;
}

typedef struct { int ctrl, shift, alt, win; WORD vk; } HotkeyDef;

static WORD keyNameToVK(const char* name) {
    size_t len = strlen(name);
    if (len == 1) {
        char c = name[0];
        if (c >= 'A' && c <= 'Z') return (WORD)c;
        if (c >= 'a' && c <= 'z') return (WORD)(c - 32);
        if (c >= '0' && c <= '9') return (WORD)c;
        switch (c) {
        case '[': return VK_OEM_4; case ']': return VK_OEM_6;
        case '\\': return VK_OEM_5; case ';': return VK_OEM_1;
        case '\'': return VK_OEM_7; case ',': return VK_OEM_COMMA;
        case '.': return VK_OEM_PERIOD; case '/': return VK_OEM_2;
        case '`': return VK_OEM_3; case '-': return VK_OEM_MINUS;
        case '=': return VK_OEM_PLUS; case '+': return VK_OEM_PLUS;
        case '*': return VK_MULTIPLY;
        }
    }
    if (len >= 2 && (name[0] == 'F' || name[0] == 'f')) {
        int num = atoi(name + 1);
        if (num >= 1 && num <= 24) return (WORD)(VK_F1 + num - 1);
    }
    
    if (strncmp(name, "NUM", 3) == 0 && len == 4) {
        switch(name[3]) {
            case '+': return VK_ADD;
            case '-': return VK_SUBTRACT;
            case '*': return VK_MULTIPLY;
            case '/': return VK_DIVIDE;
            default:
                if (name[3] >= '0' && name[3] <= '9') return (WORD)(VK_NUMPAD0 + name[3] - '0');
        }
    }

    static const struct { const char* name; WORD vk; } map[] = {
        {"Back", VK_BACK}, {"Tab", VK_TAB}, {"Enter", VK_RETURN},
        {"Space", VK_SPACE}, {"Esc", VK_ESCAPE}, {"Escape", VK_ESCAPE},
        {"Left", VK_LEFT}, {"Right", VK_RIGHT}, {"Up", VK_UP}, {"Down", VK_DOWN},
        {"Home", VK_HOME}, {"End", VK_END}, {"PgUp", VK_PRIOR}, {"PgDn", VK_NEXT},
        {"Ins", VK_INSERT}, {"Del", VK_DELETE},
    };
    for (int i = 0; i < (int)(sizeof(map)/sizeof(map[0])); i++)
        if (_stricmp(name, map[i].name) == 0) return map[i].vk; 
    return 0;
}

static int parseHotkey(const char* hk, HotkeyDef* out) {
    char buf[MAX_LEN]; strncpy(buf, hk, MAX_LEN-1); buf[MAX_LEN-1] = 0;
    out->ctrl = out->shift = out->alt = out->win = 0; out->vk = 0;
    char* p = buf;
    while (*p) {
        if (strncmp(p, "CTRL+", 5) == 0) { out->ctrl = 1; p += 5; }
        else if (strncmp(p, "SHIFT+", 6) == 0) { out->shift = 1; p += 6; }
        else if (strncmp(p, "ALT+", 4) == 0) { out->alt = 1; p += 4; }
        else if (strncmp(p, "WIN+", 4) == 0) { out->win = 1; p += 4; }
        else break;
    }
    if (!*p) return 0;
    out->vk = keyNameToVK(p);
    return out->vk != 0;
}

static void sendHotkey(const HotkeyDef* hk) {
    INPUT in[10]; int n = 0; ZeroMemory(in, sizeof(in));
    if (hk->ctrl)  { in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = VK_LCONTROL; n++; }
    if (hk->shift) { in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = VK_LSHIFT;   n++; }
    if (hk->alt)   { in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = VK_LMENU;    n++; }
    if (hk->win)   { in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = VK_LWIN;     n++; }
    in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = hk->vk; in[n].ki.dwFlags = 0; n++;
    in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = hk->vk; in[n].ki.dwFlags = KEYEVENTF_KEYUP; n++;
    if (hk->win)   { in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = VK_LWIN;     in[n].ki.dwFlags = KEYEVENTF_KEYUP; n++; }
    if (hk->alt)   { in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = VK_LMENU;    in[n].ki.dwFlags = KEYEVENTF_KEYUP; n++; }
    if (hk->shift) { in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = VK_LSHIFT;   in[n].ki.dwFlags = KEYEVENTF_KEYUP; n++; }
    if (hk->ctrl)  { in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = VK_LCONTROL; in[n].ki.dwFlags = KEYEVENTF_KEYUP; n++; }
    SendInput(n, in, sizeof(INPUT));
}

static void copyToClipboard(HWND hwnd, const char* text) {
    if (!OpenClipboard(hwnd)) return;
    EmptyClipboard();
    size_t len = strlen(text) + 1;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, len);
    if (h) { memcpy(GlobalLock(h), text, len); GlobalUnlock(h); SetClipboardData(CF_TEXT, h); }
    CloseClipboard();
}

static int getSelectedRow(void) {
    if (ListView_GetItemCount(g_list) <= 0) return -1;
    int sel = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    if (sel < 0) sel = 0;
    LVITEMA lvi = {0}; lvi.mask = LVIF_PARAM; lvi.iItem = sel;
    SendMessageA(g_list, LVM_GETITEMA, 0, (LPARAM)&lvi);
    int idx = (int)lvi.lParam;
    if (idx < 0 || idx >= g_count) return -1;
    return idx;
}

static void executeSelection(void) {
    int idx = getSelectedRow();
    if (idx < 0) return;

    if (!g_allowExec) {
        copyToClipboard(g_main, g_rows[idx].cmd);
        DestroyWindow(g_main);
        return;
    }

    HotkeyDef hk;
    if (!parseHotkey(g_rows[idx].hotkey, &hk)) {
        MessageBoxA(g_main, "无法解析该热键，请手动在 TC 中按下对应组合键。", "TC 快捷键速查", MB_ICONWARNING);
        return;
    }

    HWND tc = FindWindowA("TTOTAL_CMD", NULL);
    if (!tc) {
        MessageBoxA(g_main, "未找到 Total Commander 主窗口。", "TC 快捷键速查", MB_ICONWARNING);
        return;
    }

    g_executing = 1;
    ShowWindow(g_main, SW_HIDE);
    if (IsIconic(tc)) ShowWindow(tc, SW_RESTORE);
    
    Sleep(60);
    sendHotkey(&hk);
    DestroyWindow(g_main);
}

static void updateHeaderSort(void) {
    HWND hdr = (HWND)SendMessageA(g_list, LVM_GETHEADER, 0, 0);
    if (!hdr) return;
    static const char* base[4] = {"描述", "命令", "热键", "代码"};
    for (int i = 0; i < 4; i++) {
        char text[64];
        if (i == g_sortCol) snprintf(text, sizeof(text), "%s %s", base[i], g_sortAsc ? "▲" : "▼");
        else snprintf(text, sizeof(text), "%s", base[i]);
        HDITEMA hdi = {0}; hdi.mask = HDI_TEXT; hdi.pszText = text; hdi.cchTextMax = (int)strlen(text);
        SendMessageA(hdr, HDM_SETITEMA, i, (LPARAM)&hdi);
    }
}

static void refreshList(void) {
    ListView_DeleteAllItems(g_list);
    int n = 0;
    for (int i = 0; i < g_count; i++) {
        if (matchHotkey(g_rows[i].hotkey, g_filter, g_isSymbol))
            g_indices[n++] = i;
    }
    if (g_sortCol >= 0 && n > 1) qsort(g_indices, n, sizeof(int), compareIndices);
    g_list_count = n;

    for (int k = 0; k < n; k++) {
        int i = g_indices[k];
        LVITEMA lvi = {0}; lvi.mask = LVIF_TEXT | LVIF_PARAM; lvi.iItem = k; lvi.pszText = ""; lvi.lParam = (LPARAM)i;
        int item = (int)SendMessageA(g_list, LVM_INSERTITEMA, 0, (LPARAM)&lvi);

        ListView_SetItemText(g_list, item, 0, g_rows[i].desc);
        ListView_SetItemText(g_list, item, 1, g_rows[i].cmd);
        ListView_SetItemText(g_list, item, 2, g_rows[i].hotkey);
        ListView_SetItemText(g_list, item, 3, g_rows[i].code);
    }

    updateHeaderSort();
    char status[MAX_LEN * 2];
    const char* enterTip = g_allowExec ? "Enter 执行" : "Enter 复制";
    if (g_filterActive && g_filter[0])
        snprintf(status, sizeof(status), "过滤: %s   %s   方向键等特殊键加按Ctrl进行匹配", g_filter, enterTip);
    else
        snprintf(status, sizeof(status), "共 %d 条   按键过滤   %s   方向键等特殊键加按Ctrl进行匹配", g_count, enterTip);
    SetWindowTextA(g_status, status);
    snprintf(status, sizeof(status), "TC 快捷键速查 - 匹配 %d / %d", n, g_count);
    SetWindowTextA(g_main, status);
    if (ListView_GetItemCount(g_list) > 0)
        ListView_SetItemState(g_list, 0, LVIS_FOCUSED | LVIS_SELECTED, LVIS_FOCUSED | LVIS_SELECTED);
}

LRESULT CALLBACK ListProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR r) {
    if (m == WM_KEYDOWN || m == WM_SYSKEYDOWN) {
        if (w == VK_ESCAPE) { DestroyWindow(g_main); return 0; }
        if (w == VK_RETURN) { executeSelection(); return 0; }
        if (w == VK_F4 && (GetKeyState(VK_MENU) & 0x8000)) return DefSubclassProc(h, m, w, l);

        int ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (w == VK_UP || w == VK_DOWN || w == VK_LEFT || w == VK_RIGHT ||
            w == VK_HOME || w == VK_END || w == VK_PRIOR || w == VK_NEXT) {
            if (!ctrl) return DefSubclassProc(h, m, w, l);
        }

        char key[32];
        int t = vkToKeyStr(w, key, sizeof(key));
        if (t != 0) {
            g_isSymbol = (t == -1);
            strncpy(g_filter, key, sizeof(g_filter)-1);
            g_filter[sizeof(g_filter)-1] = 0;
            g_filterActive = 1;
            refreshList();
        }
        return 0;
    }
    if (m == WM_CHAR || m == WM_SYSCHAR) return 0;
    return DefSubclassProc(h, m, w, l);
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE:
        g_status = CreateWindowExA(0, "STATIC", "", WS_CHILD|WS_VISIBLE|SS_LEFT, 0,0,0,0, h, NULL, NULL, NULL);
        SendMessage(g_status, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);

        g_listFont = CreateFontA(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Microsoft YaHei");

        g_list = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEWA, "",
            WS_CHILD|WS_VISIBLE|LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS, 0,0,0,0, h, NULL, NULL, NULL);
        SendMessage(g_list, WM_SETFONT, (WPARAM)(g_listFont ? g_listFont : GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT);
        SetWindowSubclass(g_list, ListProc, 2, 0);
        {
            LVCOLUMNA c = {0}; c.mask = LVCF_TEXT | LVCF_WIDTH;
            c.pszText = "描述"; c.cx = 300; SendMessage(g_list, LVM_INSERTCOLUMNA, 0, (LPARAM)&c);
            c.pszText = "命令"; c.cx = 220; SendMessage(g_list, LVM_INSERTCOLUMNA, 1, (LPARAM)&c);
            c.pszText = "热键"; c.cx = 200; SendMessage(g_list, LVM_INSERTCOLUMNA, 2, (LPARAM)&c);
            c.pszText = "代码"; c.cx = 60;  SendMessage(g_list, LVM_INSERTCOLUMNA, 3, (LPARAM)&c);
        }
        refreshList();
        return 0;

    case WM_NOTIFY: {
        LPNMHDR nh = (LPNMHDR)l;
        if (nh->hwndFrom != g_list) break;

        if (nh->code == NM_CUSTOMDRAW) {
            LPNMLVCUSTOMDRAW cd = (LPNMLVCUSTOMDRAW)l;
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                int visualRow = (int)cd->nmcd.dwItemSpec;
                if (visualRow >= 0 && visualRow < g_list_count) {
                    int idx = g_indices[visualRow];
                    if (g_rows[idx].srcLevel > 1) { 
                        cd->clrText = RGB(0, 120, 215); 
                    } else {
                        cd->clrText = RGB(0, 0, 0);
                    }
                }
                return CDRF_DODEFAULT;
            }
            return CDRF_DODEFAULT;
        }
        if (nh->code == NM_DBLCLK) { 
            executeSelection(); 
            return 0; 
        }
        if (nh->code == LVN_COLUMNCLICK) {
            LPNMLISTVIEW nlv = (LPNMLISTVIEW)l;
            int col = nlv->iSubItem;
            if (col >= 1 && col <= 3) {
                if (g_sortCol == col) g_sortAsc = !g_sortAsc;
                else { g_sortCol = col; g_sortAsc = 1; }
                refreshList();
            }
            return 0;
        }
        break;
    }

    case WM_ACTIVATE:
        if (LOWORD(w) != WA_INACTIVE) {
            SetFocus(g_list);
        }
        return 0;

    case WM_SYSKEYDOWN:
        if (w == VK_F4 && (GetKeyState(VK_MENU) & 0x8000)) break;
        if (g_list) { SendMessage(g_list, WM_SYSKEYDOWN, w, l); return 0; }
        break;

    case WM_SIZE: {
        int ww = LOWORD(l), hh = HIWORD(l);
        MoveWindow(g_status, 8, 8, ww - 16, 20, TRUE);
        MoveWindow(g_list,   8, 32, ww - 16, hh - 40, TRUE);
        }
        return 0;

    case WM_GETMINMAXINFO:
        ((MINMAXINFO*)l)->ptMinTrackSize.x = 480;
        ((MINMAXINFO*)l)->ptMinTrackSize.y = 260;
        return 0;

    case WM_CLOSE:
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        if (g_listFont) { DeleteObject(g_listFont); g_listFont = NULL; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);

    int export_mode = 0;
    int export_txt_mode = 0;
    int patch_mnu_mode = 0;
    char custom_path[MAX_PATH] = {0};
    
    if (lpCmd && *lpCmd) {
        char* tok = lpCmd;
        if (strstr(tok, "--exec")) g_allowExec = 1;
        
        if (strstr(tok, "--export-txt")) export_txt_mode = 1;
        else if (strstr(tok, "--export")) export_mode = 1;
        else if (strstr(tok, "--patch")) patch_mnu_mode = 1;
        
        char* p = strstr(tok, "--path=");
        if (p) {
            p += 7;
            if (*p == '\"') {
                p++;
                char* end = strchr(p, '\"');
                if (end) {
                    int len = end - p;
                    if (len > 0 && len < MAX_PATH) {
                        strncpy(custom_path, p, len);
                        custom_path[len] = 0;
                    }
                }
            } else {
                char* end = strchr(p, ' ');
                if (end) {
                    int len = end - p;
                    if (len > 0 && len < MAX_PATH) {
                        strncpy(custom_path, p, len);
                        custom_path[len] = 0;
                    }
                } else {
                    strncpy(custom_path, p, MAX_PATH - 1);
                }
            }
            int l = strlen(custom_path);
            if (l > 0 && (custom_path[l-1] == '\\' || custom_path[l-1] == '/')) custom_path[l-1] = 0;
        }
    }

    char exePath[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    char* slash = strrchr(exePath, '\\');
    if (slash) {
        int len = (int)(slash - exePath);
        strncpy(g_exeDir, exePath, len);
        g_exeDir[len] = 0;
    }

    if (custom_path[0]) {
        strcpy(g_tcDir, custom_path);
    } else {
        DetectTcDir();
    }

    loadAllData(g_exeDir);

    if (g_count == 0) {
        char errMsg[MAX_PATH * 2];
        snprintf(errMsg, sizeof(errMsg), "未找到任何快捷键数据，请检查以下文件是否存在:\n\nTC 目录: %s\n- totalcmd.inc\n- wincmd.ini\n- language\\wcmd_chn.inc\n\n程序目录: %s\n- tc_hotkeys.txt", g_tcDir, g_exeDir);
        MessageBoxA(NULL, errMsg, "TC 快捷键速查", MB_ICONERROR);
        return 1;
    }

    if (export_mode) {
        int count = ExportMnu(g_exeDir);
        char msg[MAX_PATH * 2];
        snprintf(msg, sizeof(msg), "菜单导出成功，共导出 %d 条快捷键。\n\n请查看 exe 目录下的 output_hotkeys.mnu 文件。", count);
        MessageBoxA(NULL, msg, "导出成功", MB_OK);
        return 0;
    }

    if (export_txt_mode) {
        int count = ExportTxt(g_exeDir);
        char msg[MAX_PATH * 2];
        snprintf(msg, sizeof(msg), "快捷键清单导出成功，共导出 %d 条快捷键。\n\n请查看 exe 目录下的 output_hotkeys.txt 文件。", count);
        MessageBoxA(NULL, msg, "导出成功", MB_OK);
        return 0;
    }

    if (patch_mnu_mode) {
        int count = PatchMnu(g_exeDir);
        if (count < 0) {
            MessageBoxA(NULL, "补丁生成失败！\n\n请在 exe 目录下放置要修改的菜单文件，并命名为 input.mnu。", "文件缺失", MB_ICONERROR);
            return 1;
        }
        char msg[MAX_PATH * 2];
        snprintf(msg, sizeof(msg), "主菜单补丁生成成功，共替换了 %d 条快捷键。\n\n请查看 exe 目录下的 output_patched.mnu 文件。", count);
        MessageBoxA(NULL, msg, "补丁成功", MB_OK);
        return 0;
    }

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = WndProc; wc.hInstance = hInst; wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); 
    wc.lpszClassName = "TCHotkeySearch";
    RegisterClassA(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int ww = 833, wh = 520;

    g_hTc = FindWindowA("TTOTAL_CMD", NULL);
    HWND hOwner = g_hTc;
    DWORD exStyle = 0; 
    if (!g_hTc) {
        exStyle = WS_EX_APPWINDOW; 
    }

    g_main = CreateWindowExA(exStyle, "TCHotkeySearch", "TC 快捷键速查",
        WS_OVERLAPPEDWINDOW, (sw - ww) / 2, (sh - wh) / 2, ww, wh, hOwner, NULL, hInst, NULL);
    
    if (!g_main) return 1;
    
    ShowWindow(g_main, nShow);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessage(&msg); }
    return 0;
}