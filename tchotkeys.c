// tc-hotkeys.c
// 编译: gcc -O2 -s -mwindows -finput-charset=UTF-8 -fexec-charset=GBK -o tc-hotkeys.exe tc-hotkeys.c -lcomctl32
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ROWS 4096
#define MAX_LEN  512
#define MAX_PARTS 32

typedef struct {
    char desc[MAX_LEN];
    char cmd[MAX_LEN];
    char hotkey[MAX_LEN];
    int  srcLine;
} Row;

static Row  g_rows[MAX_ROWS];
static int  g_count = 0;
static HWND g_list, g_main, g_status;
static HFONT g_listFont = NULL;
static char g_filter[64] = {0};
static int  g_filterActive = 0;
static int  g_isSymbol = 0;
static int  g_executing = 0;
static int  g_allowExec = 0;      /* 默认安全模式：Enter 只复制 */

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

static int splitTab(char* line, char* fld[], int maxf) {
    int nf = 0;
    char* s = line;
    while (nf < maxf) {
        fld[nf++] = s;
        char* t = strchr(s, '\t');
        if (!t) break;
        *t = 0;
        s = t + 1;
    }
    return nf;
}

static int splitEq(char* s, char* parts[], int maxp) {
    int n = 0;
    char* p = s;
    while (n < maxp) {
        parts[n++] = p;
        char* eq = strstr(p, " = ");
        if (!eq) break;
        *eq = 0;
        p = eq + 3;
    }
    return n;
}

static void addHotkey(char* raw, char processed[][MAX_LEN], int* nproc) {
    char tmp[MAX_LEN];
    strncpy(tmp, raw, MAX_LEN-1);
    tmp[MAX_LEN-1] = 0;
    trimRight(tmp);
    char* t = tmp;
    while (*t == ' ') t++;
    if (!*t) return;
    if (strstr(t, "(*)")) return;
    stripMark(t);
    normalizeOem(t);
    for (int m = 0; m < *nproc; m++)
        if (strcmp(processed[m], t) == 0) return;
    if (*nproc < MAX_PARTS) {
        strncpy(processed[*nproc], t, MAX_LEN-1);
        processed[*nproc][MAX_LEN-1] = 0;
        (*nproc)++;
    }
}

static void parseText(char* text) {
    g_count = 0;
    char* line = text;
    int srcLine = 0;

    while (line && *line && g_count < MAX_ROWS) {
        char* nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char* L = line;

        if (*L && !strstr(L, "=====")) {
            char* fld[4] = {0};
            int nf = splitTab(L, fld, 4);
            while (nf < 4) fld[nf++] = (char*)"";
            for (int i = 0; i < 4; i++) trimRight(fld[i]);

            if (strcmp(fld[0], "描述") == 0 && strcmp(fld[1], "命令") == 0) {
                srcLine++;
                if (!nl) break;
                line = nl + 1;
                continue;
            }

            const char* cmd = fld[1];
            if (*cmd) {
                char descBuf[MAX_LEN];
                strncpy(descBuf, fld[0], MAX_LEN-1);
                descBuf[MAX_LEN-1] = 0;
                int dl = (int)strlen(descBuf);
                if (dl >= 1 && descBuf[0] == '"' && descBuf[dl-1] != '"')
                    memmove(descBuf, descBuf+1, dl);

                char curBuf[MAX_LEN], defBuf[MAX_LEN];
                strncpy(curBuf, fld[3], MAX_LEN-1); curBuf[MAX_LEN-1] = 0;
                strncpy(defBuf, fld[2], MAX_LEN-1); defBuf[MAX_LEN-1] = 0;

                char processed[MAX_PARTS][MAX_LEN];
                int nproc = 0;
                char* parts[MAX_PARTS];
                int np, k;

                if (curBuf[0] && strcmp(curBuf, "---") != 0) {
                    np = splitEq(curBuf, parts, MAX_PARTS);
                    for (k = 0; k < np; k++) addHotkey(parts[k], processed, &nproc);
                }
                if (defBuf[0] && strcmp(defBuf, "---") != 0) {
                    np = splitEq(defBuf, parts, MAX_PARTS);
                    for (k = 0; k < np; k++) addHotkey(parts[k], processed, &nproc);
                }

                for (k = 0; k < nproc && g_count < MAX_ROWS; k++) {
                    strncpy(g_rows[g_count].desc,   descBuf,      MAX_LEN-1);
                    strncpy(g_rows[g_count].cmd,    cmd,          MAX_LEN-1);
                    strncpy(g_rows[g_count].hotkey, processed[k], MAX_LEN-1);
                    g_rows[g_count].desc[MAX_LEN-1]   = 0;
                    g_rows[g_count].cmd[MAX_LEN-1]    = 0;
                    g_rows[g_count].hotkey[MAX_LEN-1] = 0;
                    g_rows[g_count].srcLine = srcLine;
                    g_count++;
                }
            }
        }
        srcLine++;
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

/* ================= 热键字符串 → 虚拟键码 ================= */

typedef struct { int ctrl, shift, alt, win; WORD vk; } HotkeyDef;

static WORD keyNameToVK(const char* name) {
    size_t len = strlen(name);
    if (len == 1) {
        char c = name[0];
        if (c >= 'A' && c <= 'Z') return (WORD)c;
        if (c >= 'a' && c <= 'z') return (WORD)(c - 32);
        if (c >= '0' && c <= '9') return (WORD)c;
        switch (c) {
        case '[':  return VK_OEM_4;
        case ']':  return VK_OEM_6;
        case '\\': return VK_OEM_5;
        case ';':  return VK_OEM_1;
        case '\'': return VK_OEM_7;
        case ',':  return VK_OEM_COMMA;
        case '.':  return VK_OEM_PERIOD;
        case '/':  return VK_OEM_2;
        case '`':  return VK_OEM_3;
        case '-':  return VK_OEM_MINUS;
        case '=':  return VK_OEM_PLUS;
        case '+':  return VK_OEM_PLUS;
        case '*':  return VK_MULTIPLY;
        }
    }
    if (len >= 2 && (name[0] == 'F' || name[0] == 'f')) {
        int num = atoi(name + 1);
        if (num >= 1 && num <= 24) return (WORD)(VK_F1 + num - 1);
    }
    if (strncmp(name, "Num ", 4) == 0) {
        const char* k = name + 4;
        if (strcmp(k, "+") == 0) return VK_ADD;
        if (strcmp(k, "-") == 0) return VK_SUBTRACT;
        if (strcmp(k, "*") == 0) return VK_MULTIPLY;
        if (strcmp(k, "/") == 0) return VK_DIVIDE;
    }
    static const struct { const char* name; WORD vk; } map[] = {
        {"Back", VK_BACK}, {"Tab", VK_TAB}, {"Enter", VK_RETURN},
        {"Space", VK_SPACE}, {"Esc", VK_ESCAPE}, {"Escape", VK_ESCAPE},
        {"Left", VK_LEFT}, {"Right", VK_RIGHT}, {"Up", VK_UP}, {"Down", VK_DOWN},
        {"Home", VK_HOME}, {"End", VK_END},
        {"PgUp", VK_PRIOR}, {"PgDn", VK_NEXT},
        {"Ins", VK_INSERT}, {"Del", VK_DELETE},
    };
    for (int i = 0; i < (int)(sizeof(map)/sizeof(map[0])); i++)
        if (strcmp(name, map[i].name) == 0) return map[i].vk;
    return 0;
}

static int parseHotkey(const char* hk, HotkeyDef* out) {
    char buf[MAX_LEN];
    strncpy(buf, hk, MAX_LEN-1); buf[MAX_LEN-1] = 0;
    int len = (int)strlen(buf);
    while (len > 0 && buf[len-1] == ' ') buf[--len] = 0;

    out->ctrl = out->shift = out->alt = out->win = 0;
    out->vk = 0;

    char* p = buf;
    while (*p) {
        if      (strncmp(p, "Ctrl + ",  7) == 0) { out->ctrl = 1;  p += 7; }
        else if (strncmp(p, "Shift + ", 8) == 0) { out->shift = 1; p += 8; }
        else if (strncmp(p, "Alt + ",   6) == 0) { out->alt = 1;   p += 6; }
        else if (strncmp(p, "Win + ",   6) == 0) { out->win = 1;   p += 6; }
        else break;
    }
    if (!*p) return 0;
    out->vk = keyNameToVK(p);
    return out->vk != 0;
}

/* ================= 模拟按键 ================= */

static void sendHotkey(const HotkeyDef* hk) {
    INPUT in[10];
    int n = 0;
    ZeroMemory(in, sizeof(in));

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

/* ================= 复制 / 执行 ================= */

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

static void executeSelection(void) {
    int idx = getSelectedRow();
    if (idx < 0) return;

    /* 安全模式：不执行，退化为复制命令名 */
    if (!g_allowExec) {
        copyToClipboard(g_main, g_rows[idx].cmd);
        DestroyWindow(g_main);
        return;
    }

    HotkeyDef hk;
    if (!parseHotkey(g_rows[idx].hotkey, &hk)) {
        MessageBoxA(g_main,
            "无法解析该热键，请手动在 TC 中按下对应组合键。",
            "TC 快捷键速查", MB_ICONWARNING);
        return;
    }

    HWND tc = FindWindowA("TTOTAL_CMD", NULL);
    if (!tc) {
        MessageBoxA(g_main, "未找到 Total Commander 主窗口。",
                    "TC 快捷键速查", MB_ICONWARNING);
        return;
    }

    g_executing = 1;
    ShowWindow(g_main, SW_HIDE);

    if (IsIconic(tc)) ShowWindow(tc, SW_RESTORE);
    SetForegroundWindow(tc);
    Sleep(60);

    sendHotkey(&hk);

    DestroyWindow(g_main);
}

/* ================= 刷新列表 ================= */

static void refreshList(void) {
    ListView_DeleteAllItems(g_list);
    int idx = 0;
    int lastSrcLine = -1;

    for (int i = 0; i < g_count; i++) {
        if (!matchHotkey(g_rows[i].hotkey, g_filter, g_isSymbol)) continue;

        LVITEMA lvi = {0};
        lvi.mask = LVIF_TEXT | LVIF_PARAM;
        lvi.iItem = idx;
        lvi.pszText = "";
        lvi.lParam = (LPARAM)i;
        int item = (int)SendMessageA(g_list, LVM_INSERTITEMA, 0, (LPARAM)&lvi);

        if (g_rows[i].srcLine != lastSrcLine) {
            ListView_SetItemText(g_list, item, 0, g_rows[i].desc);
            lastSrcLine = g_rows[i].srcLine;
        } else {
            ListView_SetItemText(g_list, item, 0, "");
        }
        ListView_SetItemText(g_list, item, 1, g_rows[i].cmd);
        ListView_SetItemText(g_list, item, 2, g_rows[i].hotkey);
        idx++;
    }

    char status[MAX_LEN * 2];
    const char* enterTip = g_allowExec ? "Enter 执行" : "Enter 复制";
    if (g_filterActive && g_filter[0])
        sprintf(status, "过滤: %s   %s   方向键等特殊键加按Ctrl进行匹配   双击复制命令", g_filter, enterTip);
    else
        sprintf(status, "共 %d 条   按键过滤   %s   方向键等特殊键加按Ctrl进行匹配   双击复制命令", g_count, enterTip);
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
            c.pszText = "描述"; c.cx = 320; SendMessage(g_list, LVM_INSERTCOLUMNA, 0, (LPARAM)&c);
            c.pszText = "命令"; c.cx = 240; SendMessage(g_list, LVM_INSERTCOLUMNA, 1, (LPARAM)&c);
            c.pszText = "热键"; c.cx = 220; SendMessage(g_list, LVM_INSERTCOLUMNA, 2, (LPARAM)&c);
        }
        refreshList();
        return 0;

    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE) {
            if (!g_executing) PostMessage(h, WM_CLOSE, 0, 0);
        } else {
            SetFocus(g_list);
        }
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

/* ================= 入口 ================= */

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);

    /* 解析命令行：--exec 启用执行模式，其余参数视为文件路径 */
    char path[MAX_PATH] = {0};
    if (lpCmd && *lpCmd) {
        char* tok = lpCmd;
        while (*tok) {
            while (*tok == ' ') tok++;
            if (!*tok) break;
            char* end = tok;
            int quoted = 0;
            if (*tok == '"') { quoted = 1; tok++; end = tok; while (*end && *end != '"') end++; }
            else { while (*end && *end != ' ') end++; }

            if (!quoted && strncmp(tok, "--exec", 6) == 0) {
                g_allowExec = 1;
            } else if (*tok) {
                int n = (int)(end - tok);
                if (n > 0 && n < MAX_PATH) {
                    memcpy(path, tok, n);
                    path[n] = 0;
                }
            }
            if (*end) end++;
            tok = end;
        }
    }
    if (!path[0]) {
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
    int ww = 800, wh = 420;
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