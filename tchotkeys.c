// tc-hotkeys.c
// 编译: gcc -O2 -s -mwindows -finput-charset=UTF-8 -fexec-charset=GBK -o tc-hotkeys.exe tc-hotkeys.c -lcomctl32
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ROWS 4096
#define MAX_LEN  512

typedef struct {
    char cmd[MAX_LEN];
    char hotkey[MAX_LEN];
} Row;

static Row  g_rows[MAX_ROWS];
static int  g_count = 0;
static HWND g_list, g_main, g_status;
static HFONT g_listFont = NULL;
static char g_filter[64] = {0};
static int  g_filterActive = 0;
static int  g_isSymbol = 0;

/* ================= 文件读取 / 解析 ================= */

static char* readFile(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, sz, f); buf[sz] = 0; fclose(f);
    if (sz >= 3 && (unsigned char)buf[0]==0xEF && (unsigned char)buf[1]==0xBB && (unsigned char)buf[2]==0xBF)
        memmove(buf, buf+3, sz-2);
    return buf;
}

static void trimRight(char* s) {
    int n = (int)strlen(s);
    while (n > 0 && (s[n-1]==' '||s[n-1]=='\r'||s[n-1]=='\t')) s[--n] = 0;
}

static void stripMark(char* s) {
    char* p;
    while ((p = strstr(s, " (*)")) != NULL) memmove(p, p + 4, strlen(p + 4) + 1);
    while ((p = strstr(s, "(*)"))  != NULL) memmove(p, p + 3, strlen(p + 3) + 1);
    trimRight(s);
}

static void replaceAll(char* s, const char* from, const char* to) {
    size_t fl = strlen(from), tl = strlen(to);
    if (fl == 0 || fl < tl) return;
    char* p;
    while ((p = strstr(s, from)) != NULL) {
        if (fl > tl) memmove(p + tl, p + fl, strlen(p + fl) + 1);
        memcpy(p, to, tl);
    }
}

static void normalizeOem(char* s) {
    static const struct { const char* from; const char* to; } map[] = {
        {"OEM_US[{" , "["}, {"OEM_US]}" , "]"},
        {"OEM_US\\|", "\\"}, {"OEM_US;:" , ";"},
        {"OEM_US'\"" , "'"}, {"OEM_US,<" , ","},
        {"OEM_US.>" , "."}, {"OEM_US/?" , "/"},
        {"OEM_US`~" , "`"},
        {"OEM_+"    , "+"}, {"OEM_-"    , "-"},
        {"OEM_*"    , "*"}, {"OEM_/"    , "/"},
    };
    for (size_t i = 0; i < sizeof(map)/sizeof(map[0]); i++)
        replaceAll(s, map[i].from, map[i].to);
}

/* 按 Tab 或连续 2+ 空格分列；快捷键里的 " + " 是单空格，不受影响 */
static int splitFields(char* line, char* fld[], int maxf) {
    int nf = 0;
    char* s = line;
    while (nf < maxf) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        fld[nf++] = s;
        char* p = s;
        while (*p && *p != '\t' && !(*p == ' ' && *(p + 1) == ' ')) p++;
        if (!*p) break;
        *p = 0;
        s = p + 1;
    }
    return nf;
}

static void parseText(char* text) {
    g_count = 0;
    char* line = text;
    while (line && *line && g_count < MAX_ROWS) {
        char* nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char* L = line;
        if (*L && !strstr(L, "=====") && !strstr(L, "命令")) {
            char* fld[5] = {0};
            int nf = splitFields(L, fld, 5);
            while (nf < 5) fld[nf++] = (char*)"";
            for (int i = 0; i < 5; i++) trimRight(fld[i]);

            const char* cmd = fld[0];
            const char* def = fld[2];
            const char* cur = fld[3];

            int hasDef = *def && strcmp(def, "---") != 0;
            int hasCur = *cur && strcmp(cur, "---") != 0;
            int defMarked = hasDef && strstr(def, "(*)") != NULL;

            /* 三层判断：
               cur 有效           -> 用 cur
               cur 为空, def 未标记 -> 用 def
               cur 为空, def 带(*)  -> 快捷键已被取消，丢弃 */
            const char* hk = NULL;
            if (hasCur)
                hk = cur;
            else if (hasDef && !defMarked)
                hk = def;

            if (*cmd && hk) {
                strncpy(g_rows[g_count].cmd, cmd, MAX_LEN-1);
                strncpy(g_rows[g_count].hotkey, hk, MAX_LEN-1);
                g_rows[g_count].cmd[MAX_LEN-1] = 0;
                g_rows[g_count].hotkey[MAX_LEN-1] = 0;
                stripMark(g_rows[g_count].hotkey);
                normalizeOem(g_rows[g_count].hotkey);
                g_count++;
            }
        }
        if (!nl) break;
        line = nl + 1;
    }
}

/* ================= 搜索匹配 ================= */

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

static const char* lastSpace(const char* s) {
    const char* last = s;
    for (const char* p = s; *p; p++)
        if (*p == ' ') last = p + 1;
    return last;
}

static int matchHotkey(const char* hk, const char* key, int isSymbol) {
    if (!key || !*key) return 1;
    const char* last = lastSpace(hk);
    if (isSymbol) return icontains(last, key);
    return iequals(last, key);
}

static int vkToKeyStr(WPARAM vk, char* out, int outSize) {
    out[0] = 0;
    if (vk >= VK_F1 && vk <= VK_F12) { sprintf(out, "F%d", (int)(vk - VK_F1 + 1)); return 1; }
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
    case VK_ADD:      strcpy(out, "+");     return -1;
    case VK_SUBTRACT: strcpy(out, "-");     return -1;
    case VK_MULTIPLY: strcpy(out, "*");     return -1;
    case VK_DIVIDE:   strcpy(out, "/");     return -1;
    case VK_OEM_PLUS: strcpy(out, "+");     return -1;
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

/* ================= 复制 / 执行（走 tcmc） ================= */

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
    LVITEMA lvi = {0};
    lvi.mask = LVIF_PARAM;
    lvi.iItem = sel;
    SendMessageA(g_list, LVM_GETITEMA, 0, (LPARAM)&lvi);
    int idx = (int)lvi.lParam;
    if (idx < 0 || idx >= g_count) return -1;
    return idx;
}

static void copySelectionAndClose(void) {
    int idx = getSelectedRow();
    if (idx < 0) return;
    copyToClipboard(g_main, g_rows[idx].cmd);
    DestroyWindow(g_main);
}

/* 找 tcmc.exe：优先本程序同目录，找不到就交给 PATH */
static void buildTcmcPath(char* out, int size) {
    GetModuleFileNameA(NULL, out, size);
    char* slash = strrchr(out, '\\');
    if (slash) *(slash + 1) = 0;
    strncat(out, "tcmc.exe", size - strlen(out) - 1);
    if (GetFileAttributesA(out) == INVALID_FILE_ATTRIBUTES)
        strcpy(out, "tcmc.exe");
}

static void executeSelection(void) {
    int idx = getSelectedRow();
    if (idx < 0) return;
    Row* r = &g_rows[idx];

    HWND tc = FindWindowA("TTOTAL_CMD", NULL);
    if (tc) {
        if (IsIconic(tc)) ShowWindow(tc, SW_RESTORE);
        SetForegroundWindow(tc);
    }

    char tcmc[MAX_PATH];
    buildTcmcPath(tcmc, sizeof(tcmc));

    /* em_XXX -> emem_XXX    cm_XXX -> cmcm_XXX    其他原样传 */
    char arg[MAX_LEN + 8];
    if (strncmp(r->cmd, "em_", 3) == 0)
        snprintf(arg, sizeof(arg), "em%s", r->cmd);
    else if (strncmp(r->cmd, "cm_", 3) == 0)
        snprintf(arg, sizeof(arg), "cm%s", r->cmd);
    else
        snprintf(arg, sizeof(arg), "%s", r->cmd);

    char cmdLine[MAX_PATH + MAX_LEN + 16];
    snprintf(cmdLine, sizeof(cmdLine), "\"%s\" %s", tcmc, arg);

    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = {0};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    if (!CreateProcessA(NULL, cmdLine, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        MessageBoxA(g_main,
            "无法启动 tcmc.exe\n\n"
            "请把 tcmc.exe 放在本程序同目录，或加入 PATH。",
            "TC 快捷键速查", MB_ICONWARNING);
        return;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    DestroyWindow(g_main);
}

/* ================= 刷新列表 ================= */

static void refreshList(void) {
    ListView_DeleteAllItems(g_list);
    int idx = 0;
    for (int i = 0; i < g_count; i++) {
        if (!matchHotkey(g_rows[i].hotkey, g_filter, g_isSymbol)) continue;
        LVITEMA lvi = {0};
        lvi.mask = LVIF_TEXT | LVIF_PARAM;
        lvi.iItem = idx;
        lvi.pszText = g_rows[i].cmd;
        lvi.lParam = (LPARAM)i;
        int item = (int)SendMessageA(g_list, LVM_INSERTITEMA, 0, (LPARAM)&lvi);
        ListView_SetItemText(g_list, item, 1, g_rows[i].hotkey);
        idx++;
    }
    char status[MAX_LEN * 2];
    if (g_filterActive && g_filter[0])
        sprintf(status, "过滤: %s   Enter 执行  双击复制", g_filter);
    else
        sprintf(status, "共 %d 条   按键过滤   Enter 执行  双击复制", g_count);
    SetWindowTextA(g_status, status);
    sprintf(status, "TC 快捷键速查 - 匹配 %d / %d", idx, g_count);
    SetWindowTextA(g_main, status);
    if (ListView_GetItemCount(g_list) > 0)
        ListView_SetItemState(g_list, 0, LVIS_FOCUSED | LVIS_SELECTED, LVIS_FOCUSED | LVIS_SELECTED);
}

/* ================= 子类化 ================= */

LRESULT CALLBACK ListProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR r) {
    if (m == WM_KEYDOWN || m == WM_SYSKEYDOWN) {
        if (w == VK_ESCAPE) { DestroyWindow(g_main); return 0; }
        if (w == VK_RETURN) { executeSelection(); return 0; }
        if (w == VK_F4 && (GetKeyState(VK_MENU) & 0x8000))
            return DefSubclassProc(h, m, w, l);

        int ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (w == VK_UP || w == VK_DOWN || w == VK_LEFT || w == VK_RIGHT ||
            w == VK_HOME || w == VK_END || w == VK_PRIOR || w == VK_NEXT) {
            if (!ctrl)
                return DefSubclassProc(h, m, w, l);
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
    if (m == WM_NOTIFY && ((LPNMHDR)l)->code == NM_DBLCLK) { copySelectionAndClose(); return 0; }
    return DefSubclassProc(h, m, w, l);
}

/* ================= 主窗口 ================= */

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE:
        g_status = CreateWindowExA(0, "STATIC", "", WS_CHILD|WS_VISIBLE|SS_LEFT,
            0,0,0,0, h, NULL, NULL, NULL);
        SendMessage(g_status, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);

        g_listFont = CreateFontA(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Consolas");

        g_list = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEWA, "",
            WS_CHILD|WS_VISIBLE|LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS,
            0,0,0,0, h, NULL, NULL, NULL);
        SendMessage(g_list, WM_SETFONT,
            (WPARAM)(g_listFont ? g_listFont : GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT);
        SetWindowSubclass(g_list, ListProc, 2, 0);
        {
            LVCOLUMNA c = {0};
            c.mask = LVCF_TEXT | LVCF_WIDTH;
            c.pszText = "命令"; c.cx = 280; SendMessage(g_list, LVM_INSERTCOLUMNA, 0, (LPARAM)&c);
            c.pszText = "热键"; c.cx = 300; SendMessage(g_list, LVM_INSERTCOLUMNA, 1, (LPARAM)&c);
        }
        refreshList();
        return 0;

    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE) PostMessage(h, WM_CLOSE, 0, 0);
        else SetFocus(g_list);
        return 0;

    case WM_SYSKEYDOWN:
        if (w == VK_F4 && (GetKeyState(VK_MENU) & 0x8000)) break;
        if (g_list) { SendMessage(g_list, WM_SYSKEYDOWN, w, l); return 0; }
        break;

    case WM_SIZE: {
        int ww = LOWORD(l), hh = HIWORD(l);
        MoveWindow(g_status, 8, 8, ww - 16, 22, TRUE);
        MoveWindow(g_list,   8, 34, ww - 16, hh - 42, TRUE);
        }
        return 0;

    case WM_GETMINMAXINFO:
        ((MINMAXINFO*)l)->ptMinTrackSize.x = 400;
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

/* ================= 入口 ================= */

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);

    char path[MAX_PATH] = {0};
    if (lpCmd && *lpCmd) {
        const char* p = lpCmd;
        if (*p == '"') p++;
        int i = 0;
        while (*p && *p != '"' && i < MAX_PATH - 1) path[i++] = *p++;
        path[i] = 0;
    } else {
        GetModuleFileNameA(NULL, path, MAX_PATH);
        char* slash = strrchr(path, '\\');
        if (slash) *(slash + 1) = 0;
        strcat(path, "hotkeys.txt");
    }

    char* text = readFile(path);
    if (!text) {
        MessageBoxA(NULL,
            "找不到 hotkeys.txt\n\n"
            "方法一：把导出的 txt 改名为 hotkeys.txt 放在 exe 同目录。\n"
            "方法二：把 txt 拖到 exe 上运行。",
            "TC 快捷键速查", MB_ICONINFORMATION);
        return 1;
    }
    parseText(text);
    free(text);

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "TCHotkeySearch";
    RegisterClassA(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int ww = 640, wh = 420;
    int x = (sw - ww) / 2;
    int y = (sh - wh) / 2;

    g_main = CreateWindowExA(WS_EX_TOPMOST, "TCHotkeySearch", "TC 快捷键速查",
        WS_OVERLAPPEDWINDOW, x, y, ww, wh, NULL, NULL, hInst, NULL);
    if (!g_main) return 1;
    ShowWindow(g_main, nShow);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessage(&msg); }
    return 0;
}