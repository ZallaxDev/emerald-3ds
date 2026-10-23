"""Bottom-screen taps and message pages, run from the game's own source.

Extracts by name, and compiles on the host with minimal stubs:
- src/menu.c: the tap slot (CtrMenu_PostTap/TakeTap/PostAck/TakeAck) and its
  30-frame life on gMain.vblankCounter1;
- src/text.c: CtrText_PrinterPage, with the real GetExtCtrlCodeLength from
  src/string_util.c and the TextPrinter structs from include/text.h;
- src/item_menu.c: CtrBag_LastPage;
- src/party_menu.c: CtrParty_MessagePage, over a stubbed printer page.
Character values come from include/constants/characters.h itself.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

PORT = Path(__file__).resolve().parents[1]
REPO = PORT.parent


def block(source, pattern):
    """The text from the match of pattern through its matching closing brace."""
    match = re.search(pattern, source, re.M)
    if not match:
        raise SystemExit("not found: " + pattern)
    start = match.start()
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def define(source, name):
    match = re.search(r'^#define\s+' + name + r'\s+(\S+)', source, re.M)
    if not match:
        raise SystemExit("no #define " + name)
    return match.group(1)


HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t s8;
typedef int16_t s16;
typedef uint8_t bool8;
typedef uint16_t bool16;
#define TRUE 1
#define FALSE 0
#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

#include "constants/characters.h"

#define WINDOWS_MAX @WINDOWS_MAX@
#define CTR_VIEW_TEXT @CTR_VIEW_TEXT@
#define WIN_MSG 6

static struct { u32 vblankCounter1; } gMain;

@RENDER_STATES@;
@TEMPLATE@;
@PRINTER@;

static struct TextPrinter sTextPrinters[WINDOWS_MAX];

/* An alias the port resolves to the real string, as Port_ResolveTextPointer
   does for ROM addresses. */
static const u8 sAlias[1];
static const u8 *sAliasTarget;
static const u8 *ResolveTextString(const u8 *str)
{
    return str == sAlias ? sAliasTarget : str;
}

@GETEXT@

/* ---- src/menu.c ---- */
@TAPSLOT@;
@POSTTAP@
@TAKETAP@
@POSTACK@
@TAKEACK@

/* ---- src/text.c ---- */
@PRINTERPAGE@

/* ---- src/item_menu.c ---- */
@LASTPAGE@

/* ---- src/party_menu.c ---- */
static s16 sStubPage;
static const u8 *sStubSrc;
static s16 StubPrinterPage(u8 windowId, const u8 *str)
{
    assert(windowId == WIN_MSG);
    assert(str == sStubSrc);
    return sStubPage;
}
static u8 sCtrMessage[@MSGSIZE@];
static const u8 *sCtrMessageSrc;
@MESSAGEPAGE@

/* ---- helpers ---- */
#define PC CHAR_PROMPT_CLEAR
#define FC EXT_CTRL_CODE_BEGIN

static void Fill(u8 *dst, size_t size, const u8 *src, size_t len)
{
    assert(len < size);
    memset(dst, 0xAA, size);
    memcpy(dst, src, len);
    dst[len] = EOS;
}

static size_t Len(const u8 *s)
{
    size_t n = 0;
    while (s[n] != EOS)
        n++;
    return n;
}

static int Same(const u8 *a, const u8 *b, size_t len)
{
    return Len(a) == len && memcmp(a, b, len) == 0;
}

static void TestTaps(void)
{
    s16 x = 0, y = 0;

    memset(&sCtrTap, 0, sizeof(sCtrTap));
    gMain.vblankCounter1 = 100;
    assert(!CtrMenu_TakeTap(&x, &y));
    assert(!CtrMenu_TakeAck());

    /* A plain tap is taken by TakeTap, once. */
    CtrMenu_PostTap(12, 34);
    gMain.vblankCounter1 = 130;
    assert(CtrMenu_TakeTap(&x, &y) && x == 12 && y == 34);
    assert(!CtrMenu_TakeTap(&x, &y));
    assert(!CtrMenu_TakeAck());

    /* ... and by TakeAck. */
    CtrMenu_PostTap(5, 6);
    assert(CtrMenu_TakeAck());
    assert(!CtrMenu_TakeTap(&x, &y));

    /* An ack is taken by TakeAck. */
    CtrMenu_PostAck();
    assert(CtrMenu_TakeAck());
    assert(!CtrMenu_TakeAck());

    /* An ack offered to TakeTap is dropped and consumed. */
    CtrMenu_PostAck();
    x = y = 77;
    assert(!CtrMenu_TakeTap(&x, &y));
    assert(x == 77 && y == 77);
    assert(!CtrMenu_TakeAck());

    /* Expiry: 30 frames still live, 31 dropped and consumed. */
    gMain.vblankCounter1 = 200;
    CtrMenu_PostTap(1, 2);
    gMain.vblankCounter1 = 231;
    assert(!CtrMenu_TakeTap(&x, &y));
    assert(!CtrMenu_TakeAck());
    gMain.vblankCounter1 = 200;
    CtrMenu_PostTap(1, 2);
    gMain.vblankCounter1 = 231;
    assert(!CtrMenu_TakeAck());
    assert(!CtrMenu_TakeTap(&x, &y));
    gMain.vblankCounter1 = 200;
    CtrMenu_PostAck();
    gMain.vblankCounter1 = 230;
    assert(CtrMenu_TakeAck());
    gMain.vblankCounter1 = 200;
    CtrMenu_PostAck();
    gMain.vblankCounter1 = 231;
    assert(!CtrMenu_TakeAck());

    /* The counter wrapping does not age a fresh tap. */
    gMain.vblankCounter1 = 0xFFFFFFF0u;
    CtrMenu_PostTap(3, 4);
    gMain.vblankCounter1 = 5;
    assert(CtrMenu_TakeTap(&x, &y) && x == 3 && y == 4);

    /* A tap after an ack clears the ack flag. */
    gMain.vblankCounter1 = 300;
    CtrMenu_PostAck();
    CtrMenu_PostTap(40, 50);
    assert(CtrMenu_TakeTap(&x, &y) && x == 40 && y == 50);

    /* A newer tap replaces an older one, its frame too. */
    gMain.vblankCounter1 = 400;
    CtrMenu_PostTap(1, 1);
    gMain.vblankCounter1 = 425;
    CtrMenu_PostTap(2, 2);
    gMain.vblankCounter1 = 450;
    assert(CtrMenu_TakeTap(&x, &y) && x == 2 && y == 2);
    puts("PASS tap slot: tap/ack taking, ack dropped by TakeTap, 30-frame life, wrap, ack cleared");
}

static u8 sText[0x500];

static s16 PageAt(size_t at, u8 state)
{
    memset(sTextPrinters, 0, sizeof(sTextPrinters));
    sTextPrinters[WIN_MSG].active = TRUE;
    sTextPrinters[WIN_MSG].state = state;
    sTextPrinters[WIN_MSG].printerTemplate.currentChar = sText + at;
    return CtrText_PrinterPage(WIN_MSG, sText);
}

static void TestPrinterPage(void)
{
    /* "AB\pCD\pE" */
    static const u8 msg[] = {0xBB, 0xBC, PC, 0xBD, 0xBE, PC, 0xBF};
    /* "A" {PLAY_BGM FB FB} "B\pC": the code's argument bytes are not pages. */
    static const u8 ext[] = {0xBB, FC, EXT_CTRL_CODE_PLAY_BGM, PC, PC, 0xBC, PC, 0xBD};
    /* {COLOR_HIGHLIGHT_SHADOW FB FB FB} "\pA" */
    static const u8 ext4[] = {FC, EXT_CTRL_CODE_COLOR_HIGHLIGHT_SHADOW, PC, PC, PC, PC, 0xBB};

    Fill(sText, sizeof(sText), msg, sizeof(msg));
    assert(PageAt(0, RENDER_STATE_HANDLE_CHAR) == 0);
    assert(PageAt(2, RENDER_STATE_HANDLE_CHAR) == 0);
    assert(PageAt(3, RENDER_STATE_HANDLE_CHAR) == 1);
    /* Waiting at the \p just read: the page before it is on show. */
    assert(PageAt(3, RENDER_STATE_CLEAR) == 0);
    assert(PageAt(5, RENDER_STATE_HANDLE_CHAR) == 1);
    assert(PageAt(6, RENDER_STATE_HANDLE_CHAR) == 2);
    assert(PageAt(6, RENDER_STATE_CLEAR) == 1);
    assert(PageAt(7, RENDER_STATE_HANDLE_CHAR) == 2);
    /* CLEAR on page 0 stays 0. */
    assert(PageAt(1, RENDER_STATE_CLEAR) == 0);
    /* Just past the EOS: the printer is not on this string. */
    assert(PageAt(8, RENDER_STATE_HANDLE_CHAR) == -1);

    Fill(sText, sizeof(sText), ext, sizeof(ext));
    assert(PageAt(5, RENDER_STATE_HANDLE_CHAR) == 0);
    assert(PageAt(6, RENDER_STATE_HANDLE_CHAR) == 0);
    assert(PageAt(7, RENDER_STATE_HANDLE_CHAR) == 1);
    Fill(sText, sizeof(sText), ext4, sizeof(ext4));
    assert(PageAt(5, RENDER_STATE_HANDLE_CHAR) == 0);
    assert(PageAt(6, RENDER_STATE_HANDLE_CHAR) == 1);
    /* A code cut by the EOS. */
    {
        static const u8 cut[] = {0xBB, FC};
        Fill(sText, sizeof(sText), cut, sizeof(cut));
        assert(PageAt(3, RENDER_STATE_HANDLE_CHAR) == -1);
    }

    /* Inactive printer, bad window, NULL string. */
    Fill(sText, sizeof(sText), msg, sizeof(msg));
    assert(PageAt(3, RENDER_STATE_HANDLE_CHAR) == 1);
    sTextPrinters[WIN_MSG].active = FALSE;
    assert(CtrText_PrinterPage(WIN_MSG, sText) == -1);
    sTextPrinters[WIN_MSG].active = TRUE;
    assert(CtrText_PrinterPage(WINDOWS_MAX, sText) == -1);
    assert(CtrText_PrinterPage(WIN_MSG, NULL) == -1);
    /* Another window's printer is not this one. */
    assert(CtrText_PrinterPage(WIN_MSG - 1, sText) == -1);

    /* The pointer outside [str, str + 0x400]. */
    memset(sText, 0xBB, sizeof(sText));
    sText[sizeof(sText) - 1] = EOS;
    assert(PageAt(0x400, RENDER_STATE_HANDLE_CHAR) == 0);
    assert(PageAt(0x401, RENDER_STATE_HANDLE_CHAR) == -1);
    sTextPrinters[WIN_MSG].printerTemplate.currentChar = sText + 4;
    assert(CtrText_PrinterPage(WIN_MSG, sText + 5) == -1);

    /* The string is resolved before it is compared. */
    Fill(sText, sizeof(sText), msg, sizeof(msg));
    PageAt(6, RENDER_STATE_HANDLE_CHAR);
    sAliasTarget = sText;
    assert(CtrText_PrinterPage(WIN_MSG, sAlias) == 2);
    puts("PASS CtrText_PrinterPage: \\p pages, ext codes skipped whole, CLEAR, -1 cases, resolve");
}

static void TestBagLastPage(void)
{
    u8 src[CTR_VIEW_TEXT], dst[CTR_VIEW_TEXT];

    {
        static const u8 m[] = {0xBB, PC, 0xBC, 0xBD, PC, 0xBE, 0xBF};
        Fill(src, sizeof(src), m, sizeof(m));
        CtrBag_LastPage(dst, src);
        assert(Same(dst, m + 5, 2));
    }
    {
        static const u8 m[] = {0xBB, 0xBC};
        Fill(src, sizeof(src), m, sizeof(m));
        CtrBag_LastPage(dst, src);
        assert(Same(dst, m, 2));
    }
    {
        /* A code's argument bytes that read as \p are not pages. */
        static const u8 m[] = {0xBB, PC, 0xBC, FC, EXT_CTRL_CODE_PLAY_BGM, PC, PC, 0xBD};
        Fill(src, sizeof(src), m, sizeof(m));
        CtrBag_LastPage(dst, src);
        assert(Same(dst, m + 2, sizeof(m) - 2));
    }
    {
        /* A trailing \p leaves an empty last page. */
        static const u8 m[] = {0xBB, PC};
        Fill(src, sizeof(src), m, sizeof(m));
        CtrBag_LastPage(dst, src);
        assert(Len(dst) == 0);
    }
    {
        /* No EOS in the buffer: bounded by CTR_VIEW_TEXT. */
        memset(src, 0xBB, sizeof(src));
        src[10] = PC;
        memset(dst, 0xAA, sizeof(dst));
        CtrBag_LastPage(dst, src);
        assert(Len(dst) == CTR_VIEW_TEXT - 1 - 11);
        memset(src, 0xBB, sizeof(src));
        CtrBag_LastPage(dst, src);
        assert(Len(dst) == CTR_VIEW_TEXT - 1);
    }
    puts("PASS CtrBag_LastPage: text after the last \\p, codes skipped whole, CTR_VIEW_TEXT bound");
}

static void Message(const u8 *m, size_t len, s16 page, u8 *dst)
{
    static const u8 src[1];

    Fill(sCtrMessage, sizeof(sCtrMessage), m, len);
    sCtrMessageSrc = sStubSrc = src;
    sStubPage = page;
    memset(dst, 0xAA, CTR_VIEW_TEXT);
    CtrParty_MessagePage(dst);
}

static void TestPartyMessagePage(void)
{
    u8 dst[CTR_VIEW_TEXT];
    /* "AB\pC{PLAY_BGM FB FB}D\pEF" */
    static const u8 m[] = {0xBB, 0xBC, PC, 0xBD, FC, EXT_CTRL_CODE_PLAY_BGM, PC, PC, 0xBE, PC, 0xBF, 0xC0};
    static const u8 trail[] = {0xBB, PC, 0xBC, PC};

    Message(m, sizeof(m), 0, dst);
    assert(Same(dst, m, 2));
    Message(m, sizeof(m), 1, dst);
    assert(Same(dst, m + 3, 6));
    Message(m, sizeof(m), 2, dst);
    assert(Same(dst, m + 10, 2));
    /* -1 (not printing it: printed) is the last page, so is one past the end. */
    Message(m, sizeof(m), -1, dst);
    assert(Same(dst, m + 10, 2));
    Message(m, sizeof(m), 7, dst);
    assert(Same(dst, m + 10, 2));
    /* A trailing \p's empty page is not a page. */
    Message(trail, sizeof(trail), -1, dst);
    assert(Same(dst, trail + 2, 1));
    Message(trail, sizeof(trail), 2, dst);
    assert(Same(dst, trail + 2, 1));

    /* An oversized page is cut at CTR_VIEW_TEXT - 1 ... */
    {
        static u8 big[300];
        memset(big, 0xBB, sizeof(big));
        Message(big, sizeof(big), 0, dst);
        assert(Len(dst) == CTR_VIEW_TEXT - 1);
        /* ... without splitting a control code across the cut. */
        big[CTR_VIEW_TEXT - 3] = FC;
        big[CTR_VIEW_TEXT - 2] = EXT_CTRL_CODE_COLOR_HIGHLIGHT_SHADOW;
        Message(big, sizeof(big), 0, dst);
        assert(Len(dst) == CTR_VIEW_TEXT - 3);
        /* A code that ends exactly at the limit is kept. */
        memset(big, 0xBB, sizeof(big));
        big[CTR_VIEW_TEXT - 4] = FC;
        big[CTR_VIEW_TEXT - 3] = EXT_CTRL_CODE_COLOR;
        Message(big, sizeof(big), 0, dst);
        assert(Len(dst) == CTR_VIEW_TEXT - 1);
        assert(dst[CTR_VIEW_TEXT - 4] == FC && dst[CTR_VIEW_TEXT - 3] == EXT_CTRL_CODE_COLOR);
    }
    /* A full buffer without an EOS stays inside it. */
    memset(sCtrMessage, 0xBB, sizeof(sCtrMessage));
    sCtrMessage[sizeof(sCtrMessage) - 5] = PC;
    sStubPage = -1;
    CtrParty_MessagePage(dst);
    assert(Len(dst) == 4);
    puts("PASS CtrParty_MessagePage: page 0/1/2, -1 and past-end = last, trailing \\p, cut whole codes");
}

int main(void)
{
    TestTaps();
    TestPrinterPage();
    TestBagLastPage();
    TestPartyMessagePage();
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    menu = (REPO / "src/menu.c").read_text(encoding="utf-8")
    text = (REPO / "src/text.c").read_text(encoding="utf-8")
    string_util = (REPO / "src/string_util.c").read_text(encoding="utf-8")
    item_menu = (REPO / "src/item_menu.c").read_text(encoding="utf-8")
    party_menu = (REPO / "src/party_menu.c").read_text(encoding="utf-8")
    text_h = (REPO / "include/text.h").read_text(encoding="utf-8")
    window_h = (REPO / "include/window.h").read_text(encoding="utf-8")
    views_h = (PORT / "include/3ds_game_views.h").read_text(encoding="utf-8")

    msg_size = re.search(r'\bsCtrMessage\[(0x[0-9A-Fa-f]+|\d+)\]', party_menu).group(1)
    message_page = block(party_menu, r'^static void CtrParty_MessagePage\(u8 \*dst\)')
    message_page = message_page.replace("CtrText_PrinterPage(", "StubPrinterPage(")
    parts = {
        "WINDOWS_MAX": define(window_h, "WINDOWS_MAX"),
        "CTR_VIEW_TEXT": define(views_h, "CTR_VIEW_TEXT"),
        "RENDER_STATES": block(text_h, r'^enum \{\s*RENDER_STATE_HANDLE_CHAR'),
        "TEMPLATE": block(text_h, r'^struct TextPrinterTemplate\s*$'),
        "PRINTER": block(text_h, r'^struct TextPrinter\s*$'),
        "GETEXT": block(string_util, r'^u8 GetExtCtrlCodeLength\(u8 code\)'),
        "TAPSLOT": block(menu, r'^static struct \{\s*bool8 pending, ack;') + " sCtrTap",
        "POSTTAP": block(menu, r'^void CtrMenu_PostTap\(s16 x, s16 y\)'),
        "TAKETAP": block(menu, r'^bool8 CtrMenu_TakeTap\(s16 \*x, s16 \*y\)'),
        "POSTACK": block(menu, r'^void CtrMenu_PostAck\(void\)'),
        "TAKEACK": block(menu, r'^bool8 CtrMenu_TakeAck\(void\)'),
        "PRINTERPAGE": block(text, r'^s16 CtrText_PrinterPage\(u8 windowId, const u8 \*str\)'),
        "LASTPAGE": block(item_menu, r'^static void CtrBag_LastPage\(u8 \*dst, const u8 \*src\)'),
        "MSGSIZE": msg_size,
        "MESSAGEPAGE": message_page,
    }
    source = HARNESS
    for key, value in parts.items():
        source = source.replace("@" + key + "@", value)
    assert not re.search(r'@[A-Z_]+@', source), "unfilled placeholder"
    with tempfile.TemporaryDirectory(prefix="bottom-taps-") as directory:
        harness = Path(directory) / "test.c"
        executable = Path(directory) / "test"
        harness.write_text(source, encoding="utf-8")
        subprocess.run([args.cc, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-function", "-I", str(REPO / "include"),
                        str(harness), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
    print("PASS bottom taps: tap slot, printer page, bag last page, party message page")


if __name__ == "__main__":
    main()
