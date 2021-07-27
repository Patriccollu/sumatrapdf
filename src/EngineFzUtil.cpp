/* Copyright 2021 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
#include "../mupdf/source/fitz/color-imp.h"
}

#include "utils/BaseUtil.h"
#include "utils/Archive.h"
#include "utils/ScopedWin.h"
#include "utils/FileUtil.h"
#include "utils/HtmlParserLookup.h"
#include "utils/HtmlPullParser.h"
#include "utils/TrivialHtmlParser.h"
#include "utils/WinUtil.h"
#include "utils/ZipUtil.h"

#include "AppColors.h"
#include "wingui/TreeModel.h"
#include "EngineBase.h"
#include "EngineFzUtil.h"

#include "utils/Log.h"

// extensions to Fitz that are usable for both PDF and XPS

// maximum size of a file that's entirely loaded into memory before parsed
// and displayed; larger files will be kept open while they're displayed
// so that their content can be loaded on demand in order to preserve memory
#define MAX_MEMORY_FILE_SIZE (32 * 1024 * 1024)

RectF ToRectFl(fz_rect rect) {
    return RectF::FromXY(rect.x0, rect.y0, rect.x1, rect.y1);
}

fz_rect To_fz_rect(RectF rect) {
    fz_rect result = {(float)rect.x, (float)rect.y, (float)(rect.x + rect.dx), (float)(rect.y + rect.dy)};
    return result;
}

bool IsPointInRect(fz_rect rect, fz_point pt) {
    return ToRectFl(rect).Contains(PointF(pt.x, pt.y));
}

float FzRectOverlap(fz_rect r1, fz_rect r2) {
    if (fz_is_empty_rect(r1)) {
        return 0.0f;
    }
    fz_rect isect = fz_intersect_rect(r1, r2);
    return (isect.x1 - isect.x0) * (isect.y1 - isect.y0) / ((r1.x1 - r1.x0) * (r1.y1 - r1.y0));
}

WCHAR* PdfToWstr(fz_context* ctx, pdf_obj* obj) {
    char* s = pdf_new_utf8_from_pdf_string_obj(ctx, obj);
    WCHAR* res = strconv::Utf8ToWstr(s);
    fz_free(ctx, s);
    return res;
}

// some PDF documents contain control characters in outline titles or /Info properties
// we replace them with spaces and cleanup for display with NormalizeWSInPlace()
WCHAR* PdfCleanString(WCHAR* s) {
    if (!s) {
        return nullptr;
    }
    WCHAR* curr = s;
    while (*curr) {
        WCHAR c = *curr;
        if (c < 0x20) {
            *curr = ' ';
        }
        curr++;
    }
    str::NormalizeWSInPlace(s);
    return s;
}

fz_matrix FzCreateViewCtm(fz_rect mediabox, float zoom, int rotation) {
    fz_matrix ctm = fz_pre_scale(fz_rotate((float)rotation), zoom, zoom);

    CrashIf(0 != mediabox.x0 || 0 != mediabox.y0);
    rotation = (rotation + 360) % 360;
    if (90 == rotation) {
        ctm = fz_pre_translate(ctm, 0, -mediabox.y1);
    } else if (180 == rotation) {
        ctm = fz_pre_translate(ctm, -mediabox.x1, -mediabox.y1);
    } else if (270 == rotation) {
        ctm = fz_pre_translate(ctm, -mediabox.x1, 0);
    }

    CrashIf(fz_matrix_expansion(ctm) <= 0);
    if (fz_matrix_expansion(ctm) == 0) {
        return fz_identity;
    }

    return ctm;
}

struct istream_filter {
    IStream* stream;
    u8 buf[4096];
};

extern "C" int next_istream(fz_context* ctx, fz_stream* stm, __unused size_t max) {
    istream_filter* state = (istream_filter*)stm->state;
    ULONG cbRead = sizeof(state->buf);
    HRESULT res = state->stream->Read(state->buf, sizeof(state->buf), &cbRead);
    if (FAILED(res)) {
        fz_throw(ctx, FZ_ERROR_GENERIC, "IStream read error: %x", res);
    }
    stm->rp = state->buf;
    stm->wp = stm->rp + cbRead;
    stm->pos += cbRead;

    return cbRead > 0 ? *stm->rp++ : EOF;
}

extern "C" void seek_istream(fz_context* ctx, fz_stream* stm, i64 offset, int whence) {
    istream_filter* state = (istream_filter*)stm->state;
    LARGE_INTEGER off;
    ULARGE_INTEGER n;
    off.QuadPart = offset;
    HRESULT res = state->stream->Seek(off, whence, &n);
    if (FAILED(res)) {
        fz_throw(ctx, FZ_ERROR_GENERIC, "IStream seek error: %x", res);
    }
    if (n.HighPart != 0 || n.LowPart > INT_MAX) {
        fz_throw(ctx, FZ_ERROR_GENERIC, "documents beyond 2GB aren't supported");
    }
    stm->pos = n.LowPart;
    stm->rp = stm->wp = state->buf;
}

extern "C" void drop_istream(fz_context* ctx, void* state_) {
    istream_filter* state = (istream_filter*)state_;
    state->stream->Release();
    fz_free(ctx, state);
}

fz_stream* FzOpenIStream(fz_context* ctx, IStream* stream) {
    if (!stream) {
        return nullptr;
    }

    LARGE_INTEGER zero = {0};
    HRESULT res = stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    if (FAILED(res)) {
        fz_throw(ctx, FZ_ERROR_GENERIC, "IStream seek error: %x", res);
    }

    istream_filter* state = fz_malloc_struct(ctx, istream_filter);
    state->stream = stream;
    stream->AddRef();

    fz_stream* stm = fz_new_stream(ctx, state, next_istream, drop_istream);
    stm->seek = seek_istream;
    return stm;
}

static void* FzMemdup(fz_context* ctx, void* p, size_t size) {
    void* res = fz_malloc_no_throw(ctx, size);
    if (!res) {
        return nullptr;
    }
    memcpy(res, p, size);
    return res;
}

fz_stream* FzOpenFile2(fz_context* ctx, const WCHAR* filePath) {
    fz_stream* stm = nullptr;
    auto path = ToUtf8Temp(filePath);
    i64 fileSize = file::GetSize(path.AsView());
    // load small files entirely into memory so that they can be
    // overwritten even by programs that don't open files with FILE_SHARE_READ
    if (fileSize > 0 && fileSize < MAX_MEMORY_FILE_SIZE) {
        auto dataTmp = file::ReadFileWithAllocator(filePath, nullptr);
        if (dataTmp.empty()) {
            // failed to read
            return nullptr;
        }

        // TODO: we copy so that the memory ends up in chunk allocated
        // by libmupdf so that it works across dll boundaries.
        // We can either use  fz_new_buffer_from_shared_data
        // and free the data on the side or create Allocator that
        // uses fz_malloc_no_throw and pass it to ReadFileWithAllocator
        size_t size = dataTmp.size();
        void* data = FzMemdup(ctx, (void*)dataTmp.data(), size);
        if (!data) {
            return nullptr;
        }
        str::Free(dataTmp.data());

        fz_buffer* buf = fz_new_buffer_from_data(ctx, (u8*)data, size);
        fz_var(buf);
        fz_try(ctx) {
            stm = fz_open_buffer(ctx, buf);
        }
        fz_always(ctx) {
            fz_drop_buffer(ctx, buf);
        }
        fz_catch(ctx) {
            stm = nullptr;
        }
        return stm;
    }

    fz_try(ctx) {
        stm = fz_open_file_w(ctx, filePath);
    }
    fz_catch(ctx) {
        stm = nullptr;
    }
    return stm;
}

std::span<u8> FzExtractStreamData(fz_context* ctx, fz_stream* stream) {
    fz_seek(ctx, stream, 0, 2);
    i64 fileLen = fz_tell(ctx, stream);
    fz_seek(ctx, stream, 0, 0);

    fz_buffer* buf = fz_read_all(ctx, stream, fileLen);

    u8* data = nullptr;
    size_t size = fz_buffer_extract(ctx, buf, &data);
    CrashIf((size_t)fileLen != size);
    fz_drop_buffer(ctx, buf);
    if (!data || size == 0) {
        return {};
    }
    // this was allocated inside mupdf, make a copy that can be free()d
    u8* res = (u8*)memdup(data, size);
    fz_free(ctx, data);
    return {res, size};
}

void FzStreamFingerprint(fz_context* ctx, fz_stream* stm, u8 digest[16]) {
    i64 fileLen = -1;
    fz_buffer* buf = nullptr;

    fz_try(ctx) {
        fz_seek(ctx, stm, 0, 2);
        fileLen = fz_tell(ctx, stm);
        fz_seek(ctx, stm, 0, 0);
        buf = fz_read_all(ctx, stm, fileLen);
    }
    fz_catch(ctx) {
        fz_warn(ctx, "couldn't read stream data, using a nullptr fingerprint instead");
        ZeroMemory(digest, 16);
        return;
    }
    CrashIf(nullptr == buf);
    u8* data;
    size_t size = fz_buffer_extract(ctx, buf, &data);
    CrashIf((size_t)fileLen != size);
    fz_drop_buffer(ctx, buf);

    fz_md5 md5;
    fz_md5_init(&md5);
    fz_md5_update(&md5, data, size);
    fz_md5_final(&md5, digest);
}

// try to produce an 8-bit palette for saving some memory
static RenderedBitmap* TryRenderAsPaletteImage(fz_pixmap* pixmap) {
    int w = pixmap->w;
    int h = pixmap->h;
    int rows8 = ((w + 3) / 4) * 4;
    u8* bmpData = (u8*)calloc(rows8, h);
    if (!bmpData) {
        return nullptr;
    }

    ScopedMem<BITMAPINFO> bmi((BITMAPINFO*)calloc(1, sizeof(BITMAPINFO) + 255 * sizeof(RGBQUAD)));

    u8* dest = bmpData;
    u8* source = pixmap->samples;
    u32* palette = (u32*)bmi.Get()->bmiColors;
    u8 grayIdxs[256] = {0};

    int paletteSize = 0;
    RGBQUAD c;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            c.rgbRed = *source++;
            c.rgbGreen = *source++;
            c.rgbBlue = *source++;
            c.rgbReserved = 0;
            source++;

            /* find this color in the palette */
            int k;
            bool isGray = c.rgbRed == c.rgbGreen && c.rgbRed == c.rgbBlue;
            if (isGray) {
                k = grayIdxs[c.rgbRed] || palette[0] == *(u32*)&c ? grayIdxs[c.rgbRed] : paletteSize;
            } else {
                for (k = 0; k < paletteSize && palette[k] != *(u32*)&c; k++) {
                    ;
                }
            }
            /* add it to the palette if it isn't in there and if there's still space left */
            if (k == paletteSize) {
                if (++paletteSize > 256) {
                    free(bmpData);
                    return nullptr;
                }
                if (isGray) {
                    grayIdxs[c.rgbRed] = (BYTE)k;
                }
                palette[k] = *(u32*)&c;
            }
            /* 8-bit data consists of indices into the color palette */
            *dest++ = k;
        }
        dest += rows8 - w;
    }

    BITMAPINFOHEADER* bmih = &bmi.Get()->bmiHeader;
    bmih->biSize = sizeof(*bmih);
    bmih->biWidth = w;
    bmih->biHeight = -h;
    bmih->biPlanes = 1;
    bmih->biCompression = BI_RGB;
    bmih->biBitCount = 8;
    bmih->biSizeImage = h * rows8;
    bmih->biClrUsed = paletteSize;

    void* data = nullptr;
    HANDLE hMap = CreateFileMapping(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, bmih->biSizeImage, nullptr);
    HBITMAP hbmp = CreateDIBSection(nullptr, bmi, DIB_RGB_COLORS, &data, hMap, 0);
    if (!hbmp) {
        free(bmpData);
        return nullptr;
    }
    memcpy(data, bmpData, bmih->biSizeImage);
    free(bmpData);
    return new RenderedBitmap(hbmp, Size(w, h), hMap);
}

// had to create a copy of fz_convert_pixmap to ensure we always get the alpha
fz_pixmap* FzConvertPixmap2(fz_context* ctx, fz_pixmap* pix, fz_colorspace* ds, fz_colorspace* prf,
                              fz_default_colorspaces* default_cs, fz_color_params color_params, int keep_alpha) {
    fz_pixmap* cvt;

    if (!ds && !keep_alpha) {
        fz_throw(ctx, FZ_ERROR_GENERIC, "cannot both throw away and keep alpha");
    }

    cvt = fz_new_pixmap(ctx, ds, pix->w, pix->h, pix->seps, keep_alpha);

    cvt->xres = pix->xres;
    cvt->yres = pix->yres;
    cvt->x = pix->x;
    cvt->y = pix->y;
    if (pix->flags & FZ_PIXMAP_FLAG_INTERPOLATE) {
        cvt->flags |= FZ_PIXMAP_FLAG_INTERPOLATE;
    } else {
        cvt->flags &= ~FZ_PIXMAP_FLAG_INTERPOLATE;
    }

    fz_try(ctx) {
        fz_convert_pixmap_samples(ctx, pix, cvt, prf, default_cs, color_params, 1);
    }
    fz_catch(ctx) {
        fz_drop_pixmap(ctx, cvt);
        fz_rethrow(ctx);
    }

    return cvt;
}

RenderedBitmap* NewRenderedFzPixmap(fz_context* ctx, fz_pixmap* pixmap) {
    if (pixmap->n == 4 && fz_colorspace_is_rgb(ctx, pixmap->colorspace)) {
        RenderedBitmap* res = TryRenderAsPaletteImage(pixmap);
        if (res) {
            return res;
        }
    }

    ScopedMem<BITMAPINFO> bmi((BITMAPINFO*)calloc(1, sizeof(BITMAPINFO) + 255 * sizeof(RGBQUAD)));

    fz_pixmap* bgrPixmap = nullptr;
    fz_var(bgrPixmap);

    /* BGRA is a GDI compatible format */
    fz_try(ctx) {
        fz_colorspace* csdest = fz_device_bgr(ctx);
        fz_color_params cp = fz_default_color_params;
        bgrPixmap = FzConvertPixmap2(ctx, pixmap, csdest, nullptr, nullptr, cp, 1);
    }
    fz_catch(ctx) {
        return nullptr;
    }

    if (!bgrPixmap || !bgrPixmap->samples) {
        return nullptr;
    }

    int w = bgrPixmap->w;
    int h = bgrPixmap->h;
    int n = bgrPixmap->n;
    int imgSize = bgrPixmap->stride * h;
    int bitsCount = n * 8;

    BITMAPINFOHEADER* bmih = &bmi.Get()->bmiHeader;
    bmih->biSize = sizeof(*bmih);
    bmih->biWidth = w;
    bmih->biHeight = -h;
    bmih->biPlanes = 1;
    bmih->biCompression = BI_RGB;
    bmih->biBitCount = bitsCount;
    bmih->biSizeImage = imgSize;
    bmih->biClrUsed = 0;

    void* data = nullptr;
    HANDLE hFile = INVALID_HANDLE_VALUE;
    DWORD fl = PAGE_READWRITE;
    HANDLE hMap = CreateFileMappingW(hFile, nullptr, fl, 0, imgSize, nullptr);
    uint usage = DIB_RGB_COLORS;
    HBITMAP hbmp = CreateDIBSection(nullptr, bmi, usage, &data, hMap, 0);
    if (data) {
        u8* samples = bgrPixmap->samples;
        memcpy(data, samples, imgSize);
    }
    fz_drop_pixmap(ctx, bgrPixmap);
    if (!hbmp) {
        return nullptr;
    }
    // return a RenderedBitmap even if hbmp is nullptr so that callers can
    // distinguish rendering errors from GDI resource exhaustion
    // (and in the latter case retry using smaller target rectangles)
    return new RenderedBitmap(hbmp, Size(w, h), hMap);
}

static inline int WcharsPerRune(int rune) {
    if (rune & 0x1F0000) {
        return 2;
    }
    return 1;
}

static void AddChar(fz_stext_line* line, fz_stext_char* c, str::WStr& s, Vec<Rect>& rects) {
    fz_rect bbox = fz_rect_from_quad(c->quad);
    Rect r = ToRectFl(bbox).Round();

    int n = WcharsPerRune(c->c);
    if (n == 2) {
        WCHAR tmp[2];
        tmp[0] = 0xD800 | ((c->c - 0x10000) >> 10) & 0x3FF;
        tmp[1] = 0xDC00 | (c->c - 0x10000) & 0x3FF;
        s.Append(tmp, 2);
        rects.Append(r);
        rects.Append(r);
        return;
    }
    WCHAR wc = c->c;
    bool isNonPrintable = (wc <= 32) || str::IsNonCharacter(wc);
    if (!isNonPrintable) {
        s.Append(wc);
        rects.Append(r);
        return;
    }

    // non-printable or whitespace
    if (!str::IsWs(wc)) {
        s.Append(L'?');
        rects.Append(r);
        return;
    }

    // collapse multiple whitespace characters into one
    WCHAR prev = s.LastChar();
    if (!str::IsWs(prev)) {
        s.Append(L' ');
        rects.Append(r);
    }
}

static void AddLineSep(str::WStr& s, Vec<Rect>& rects, const WCHAR* lineSep, size_t lineSepLen) {
    if (lineSepLen == 0) {
        return;
    }
    // remove trailing spaces
    if (str::IsWs(s.LastChar())) {
        s.RemoveLast();
        rects.RemoveLast();
    }

    s.Append(lineSep);
    for (size_t i = 0; i < lineSepLen; i++) {
        rects.Append(Rect());
    }
}

WCHAR* FzTextPageToStr(fz_stext_page* text, Rect** coordsOut) {
    const WCHAR* lineSep = L"\n";

    size_t lineSepLen = str::Len(lineSep);
    str::WStr content;
    // coordsOut is optional but we ask for it by default so we simplify the code
    // by always calculating it
    Vec<Rect> rects;

    fz_stext_block* block = text->first_block;
    while (block) {
        if (block->type != FZ_STEXT_BLOCK_TEXT) {
            block = block->next;
            continue;
        }
        fz_stext_line* line = block->u.t.first_line;
        while (line) {
            fz_stext_char* c = line->first_char;
            while (c) {
                AddChar(line, c, content, rects);
                c = c->next;
            }
            AddLineSep(content, rects, lineSep, lineSepLen);
            line = line->next;
        }

        block = block->next;
    }

    CrashIf(content.size() != rects.size());

    if (coordsOut) {
        *coordsOut = rects.StealData();
    }

    return content.StealData();
}

// copy of fz_is_external_link without ctx
int IsExternalLink(const char* uri) {
    while (*uri >= 'a' && *uri <= 'z') {
        ++uri;
    }
    return uri[0] == ':';
}

static bool LinkifyCheckMultiline(const WCHAR* pageText, const WCHAR* pos, Rect* coords) {
    // multiline links end in a non-alphanumeric character and continue on a line
    // that starts left and only slightly below where the current line ended
    // (and that doesn't start with http or a footnote numeral)
    return '\n' == *pos && pos > pageText && *(pos + 1) && !iswalnum(pos[-1]) && !str::IsWs(pos[1]) &&
           coords[pos - pageText + 1].BR().y > coords[pos - pageText - 1].y &&
           coords[pos - pageText + 1].y <= coords[pos - pageText - 1].BR().y + coords[pos - pageText - 1].dy * 0.35 &&
           coords[pos - pageText + 1].x < coords[pos - pageText - 1].BR().x &&
           coords[pos - pageText + 1].dy >= coords[pos - pageText - 1].dy * 0.85 &&
           coords[pos - pageText + 1].dy <= coords[pos - pageText - 1].dy * 1.2 && !str::StartsWith(pos + 1, L"http");
}

static bool EndsURL(WCHAR c) {
    if (c == 0 || str::IsWs(c)) {
        return true;
    }
    // https://github.com/sumatrapdfreader/sumatrapdf/issues/1313
    // 0xff0c is ","
    if (c == (WCHAR)0xff0c) {
        return true;
    }
    return false;
}

static const WCHAR* LinkifyFindEnd(const WCHAR* start, WCHAR prevChar) {
    const WCHAR* quote = nullptr;

    // look for the end of the URL (ends in a space preceded maybe by interpunctuation)
    const WCHAR* end = start;
    while (!EndsURL(*end)) {
        end++;
    }
    char prev = 0;
    if (end > start) {
        prev = end[-1];
    }
    if (',' == prev || '.' == prev || '?' == prev || '!' == prev) {
        end--;
    }

    prev = 0;
    if (end > start) {
        prev = end[-1];
    }
    // also ignore a closing parenthesis, if the URL doesn't contain any opening one
    if (')' == prev && (!str::FindChar(start, '(') || str::FindChar(start, '(') >= end)) {
        end--;
    }

    // cut the link at the first quotation mark, if it's also preceded by one
    if (('"' == prevChar || '\'' == prevChar) && (quote = str::FindChar(start, prevChar)) != nullptr && quote < end) {
        end = quote;
    }

    return end;
}

static const WCHAR* LinkifyMultilineText(LinkRectList* list, const WCHAR* pageText, const WCHAR* start,
                                         const WCHAR* next, Rect* coords) {
    size_t lastIx = list->coords.size() - 1;
    AutoFreeWstr uri(list->links.at(lastIx));
    const WCHAR* end = next;
    bool multiline = false;

    do {
        end = LinkifyFindEnd(next, start > pageText ? start[-1] : ' ');
        multiline = LinkifyCheckMultiline(pageText, end, coords);

        AutoFreeWstr part(str::Dup(next, end - next));
        uri.Set(str::Join(uri, part));
        Rect bbox = coords[next - pageText].Union(coords[end - pageText - 1]);
        list->coords.Append(To_fz_rect(ToRectFl(bbox)));

        next = end + 1;
    } while (multiline);

    // update the link URL for all partial links
    list->links.at(lastIx) = str::Dup(uri);
    for (size_t i = lastIx + 1; i < list->coords.size(); i++) {
        list->links.Append(str::Dup(uri));
    }

    return end;
}

// cf. http://weblogs.mozillazine.org/gerv/archives/2011/05/html5_email_address_regexp.html
inline bool IsEmailUsernameChar(WCHAR c) {
    // explicitly excluding the '/' from the list, as it is more
    // often part of a URL or path than of an email address
    return iswalnum(c) || c && str::FindChar(L".!#$%&'*+=?^_`{|}~-", c);
}
inline bool IsEmailDomainChar(WCHAR c) {
    return iswalnum(c) || '-' == c;
}

static const WCHAR* LinkifyFindEmail(const WCHAR* pageText, const WCHAR* at) {
    const WCHAR* start;
    for (start = at; start > pageText && IsEmailUsernameChar(*(start - 1)); start--) {
        // do nothing
    }
    return start != at ? start : nullptr;
}

static const WCHAR* LinkifyEmailAddress(const WCHAR* start) {
    const WCHAR* end;
    for (end = start; IsEmailUsernameChar(*end); end++) {
        ;
    }
    if (end == start || *end != '@' || !IsEmailDomainChar(*(end + 1))) {
        return nullptr;
    }
    for (end++; IsEmailDomainChar(*end); end++) {
        ;
    }
    if ('.' != *end || !IsEmailDomainChar(*(end + 1))) {
        return nullptr;
    }
    do {
        for (end++; IsEmailDomainChar(*end); end++) {
            ;
        }
    } while ('.' == *end && IsEmailDomainChar(*(end + 1)));
    return end;
}

// caller needs to delete the result
// TODO: return Vec<IPageElement*> directly
LinkRectList* LinkifyText(const WCHAR* pageText, Rect* coords) {
    LinkRectList* list = new LinkRectList;

    for (const WCHAR* start = pageText; *start; start++) {
        const WCHAR* end = nullptr;
        bool multiline = false;
        const WCHAR* protocol = nullptr;

        if ('@' == *start) {
            // potential email address without mailto:
            const WCHAR* email = LinkifyFindEmail(pageText, start);
            end = email ? LinkifyEmailAddress(email) : nullptr;
            protocol = L"mailto:";
            if (end != nullptr) {
                start = email;
            }
        } else if (start > pageText && ('/' == start[-1] || iswalnum(start[-1]))) {
            // hyperlinks must not be preceded by a slash (indicates a different protocol)
            // or an alphanumeric character (indicates part of a different protocol)
        } else if ('h' == *start && str::Parse(start, L"http%?s://")) {
            end = LinkifyFindEnd(start, start > pageText ? start[-1] : ' ');
            multiline = LinkifyCheckMultiline(pageText, end, coords);
        } else if ('w' == *start && str::StartsWith(start, L"www.")) {
            end = LinkifyFindEnd(start, start > pageText ? start[-1] : ' ');
            multiline = LinkifyCheckMultiline(pageText, end, coords);
            protocol = L"http://";
            // ignore www. links without a top-level domain
            if (end - start <= 4 || !multiline && (!wcschr(start + 5, '.') || wcschr(start + 5, '.') >= end)) {
                end = nullptr;
            }
        } else if ('m' == *start && str::StartsWith(start, L"mailto:")) {
            end = LinkifyEmailAddress(start + 7);
        }
        if (!end) {
            continue;
        }

        AutoFreeWstr part(str::Dup(start, end - start));
        WCHAR* uri = protocol ? str::Join(protocol, part) : part.StealData();
        list->links.Append(uri);
        Rect bbox = coords[start - pageText].Union(coords[end - pageText - 1]);
        list->coords.Append(To_fz_rect(ToRectFl(bbox)));
        if (multiline) {
            end = LinkifyMultilineText(list, pageText, start, end + 1, coords);
        }

        start = end;
    }

    return list;
}

static char* PdfLinkGetURI(fz_link* link, fz_outline* outline) {
    if (link) {
        return link->uri;
    }
    if (outline) {
        return outline->uri;
    }
    return nullptr;
}

static Kind CalcDestKind(fz_link* link, fz_outline* outline) {
    // outline entries with page set to -1 go nowhere
    // see https://github.com/sumatrapdfreader/sumatrapdf/issues/1352
    if (outline && outline->page == -1) {
        return kindDestinationNone;
    }
    char* uri = PdfLinkGetURI(link, outline);
    // some outline entries are bad (issue 1245)
    if (!uri) {
        return kindDestinationNone;
    }
    if (!IsExternalLink(uri)) {
        float x = 0, y = 0, zoom = 0;
        int pageNo = resolve_link(uri, &x, &y, &zoom);
        if (pageNo == -1) {
            // TODO: figure out what it could be
            logf("CalcDestKind(): unknown uri: '%s'\n", uri);
            // ReportIf(true);
            return nullptr;
        }
        return kindDestinationScrollTo;
    }
    if (str::StartsWith(uri, "file:")) {
        // TODO: investigate more, happens in pier-EsugAwards2007.pdf
        return kindDestinationLaunchFile;
    }
    // TODO: hackish way to detect uris of various kinds
    // like http:, news:, mailto:, tel: etc.
    if (str::FindChar(uri, ':') != nullptr) {
        return kindDestinationLaunchURL;
    }

    logf("CalcDestKind(): unknown uri: '%s'\n", uri);
    // TODO: kindDestinationLaunchEmbedded, kindDestinationLaunchURL, named destination
    // ReportIf(true);
    return nullptr;
}

static WCHAR* CalcValue(fz_link* link, fz_outline* outline) {
    char* uri = PdfLinkGetURI(link, outline);
    if (!uri) {
        return nullptr;
    }
    if (!IsExternalLink(uri)) {
        // other values: #1,115,208
        return nullptr;
    }
    WCHAR* path = strconv::Utf8ToWstr(uri);
    return path;
}

static WCHAR* CalcDestName(fz_link* link, fz_outline* outline) {
    char* uri = PdfLinkGetURI(link, outline);
    if (!uri) {
        return nullptr;
    }
    if (IsExternalLink(uri)) {
        return nullptr;
    }
    // TODO(port): test with more stuff
    // figure out what PDF_NAME(GoToR) ends up being
    return strconv::Utf8ToWstr(uri);
}

static int CalcDestPageNo(fz_link* link, fz_outline* outline) {
    char* uri = PdfLinkGetURI(link, outline);
    // TODO: happened in ug_logodesign.pdf. investigate
    // CrashIf(!uri);
    if (!uri) {
        return 0;
    }
    if (IsExternalLink(uri)) {
        return 0;
    }
    float x, y;
    int pageNo = resolve_link(uri, &x, &y, nullptr);
    if (pageNo == -1) {
        return 0;
    }
    return pageNo + 1; // TODO(port): or is it just pageNo?
#if 0
    if (link && FZ_LINK_GOTO == link->kind)
        return link->ld.gotor.page + 1;
    if (link && FZ_LINK_GOTOR == link->kind && !link->ld.gotor.dest)
        return link->ld.gotor.page + 1;
#endif
    return 0;
}

static RectF CalcDestRect(fz_link* link, fz_outline* outline) {
    RectF result(DEST_USE_DEFAULT, DEST_USE_DEFAULT, DEST_USE_DEFAULT, DEST_USE_DEFAULT);
    char* uri = PdfLinkGetURI(link, outline);
    // TODO: this happens in pdf/ug_logodesign.pdf, there's only outline without
    // pageno. need to investigate
    // CrashIf(!uri);
    if (!uri) {
        return result;
    }

    if (IsExternalLink(uri)) {
        return result;
    }
    float x = 0;
    float y = 0;
    int pageNo = resolve_link(uri, &x, &y, nullptr);
    if (pageNo == -1) {
        // SubmitBugReportIf(pageNo == -1);
        return result;
    }

    result.x = (double)x;
    result.y = (double)y;
    return result;
}

static PageDestination* NewPageDestination(fz_link* link, fz_outline* outline) {
    auto dest = new PageDestination();
    dest->kind = CalcDestKind(link, outline);
    CrashIf(!dest->kind);
    if (dest->kind == kindDestinationScrollTo) {
        char* uri = PdfLinkGetURI(link, outline);
        float x = 0, y = 0, zoom = 0;
        int pageNo = resolve_link(uri, &x, &y, &zoom);
        dest->pageNo = pageNo + 1;
        dest->rect = RectF(x, y, x, y);
        dest->value = strconv::Utf8ToWstr(uri);
        dest->name = strconv::Utf8ToWstr(uri);
        dest->zoom = zoom;
    } else {
        // TODO: clean this up
        dest->rect = CalcDestRect(link, outline);
        dest->value = CalcValue(link, outline);
        dest->name = CalcDestName(link, outline);
        dest->pageNo = CalcDestPageNo(link, outline);
    }
    if ((dest->pageNo <= 0) && (dest->kind != kindDestinationNone) && (dest->kind != kindDestinationLaunchFile) &&
        (dest->kind != kindDestinationLaunchURL) && (dest->kind != kindDestinationLaunchEmbedded)) {
        logf("dest->kind: %s, dest->pageNo: %d\n", dest->kind, dest->pageNo);
        // ReportIf(dest->pageNo <= 0);
    }
    return dest;
}

PageDestination* NewFzDestination(fz_outline* outline) {
    return NewPageDestination(nullptr, outline);
}

static PageElement* NewFzLink(int srcPageNo, fz_link* link, fz_outline* outline) {
    auto res = new PageElement();
    res->kind_ = kindPageElementDest;
    res->pageNo = srcPageNo;

    if (link) {
        res->rect = ToRectFl(link->rect);
    }

    res->dest = NewPageDestination(link, outline);
    res->pageNo = res->dest->pageNo;
    res->value = str::Dup(res->dest->value);

    return res;
}

PageElement* NewFzImage(int pageNo, fz_rect rect, size_t imageIdx) {
    auto res = new PageElement();
    res->kind_ = kindPageElementImage;
    res->pageNo = pageNo;
    res->rect = ToRectFl(rect);
    res->imageID = (int)imageIdx;
    return res;
}

TocItem* NewTocItemWithDestination(TocItem* parent, WCHAR* title, PageDestination* dest) {
    auto res = new TocItem(parent, title, 0);
    res->dest = dest;
    return res;
}

IPageElement* FzGetElementAtPos(FzPageInfo* pageInfo, PointF pt) {
    if (!pageInfo) {
        return nullptr;
    }
    int pageNo = pageInfo->pageNo;
    fz_link* link = pageInfo->links;
    fz_point p = {(float)pt.x, (float)pt.y};
    while (link) {
        if (IsPointInRect(link->rect, p)) {
            return NewFzLink(pageNo, link, nullptr);
        }
        link = link->next;
    }

    for (auto* pel : pageInfo->autoLinks) {
        if (pel->GetRect().Contains(pt)) {
            return ClonePageElement(pel);
        }
    }

    for (auto* pel : pageInfo->comments) {
        if (pel->GetRect().Contains(pt)) {
            return ClonePageElement(pel);
        }
    }

    size_t imageIdx = 0;
    for (auto& img : pageInfo->images) {
        fz_rect ir = img.rect;
        if (IsPointInRect(ir, p)) {
            return NewFzImage(pageNo, ir, imageIdx);
        }
        imageIdx++;
    }
    return nullptr;
}

// TODO: construct this only once per page and change the API
// to not free the result of GetElements()
void FzGetElements(Vec<IPageElement*>* els, FzPageInfo* pageInfo) {
    if (!pageInfo) {
        return;
    }

    // since all elements lists are in last-to-first order, append
    // item types in inverse order and reverse the whole list at the end
    int pageNo = pageInfo->pageNo;

    size_t imageIdx = 0;
    for (auto& img : pageInfo->images) {
        fz_rect ir = img.rect;
        auto image = NewFzImage(pageNo, ir, imageIdx);
        els->Append(image);
        imageIdx++;
    }

    fz_link* link = pageInfo->links;
    while (link) {
        auto el = NewFzLink(pageNo, link, nullptr);
        els->Append(el);
        link = link->next;
    }

    for (auto&& pel : pageInfo->autoLinks) {
        auto el = ClonePageElement(pel);
        els->Append(el);
    }

    for (auto* comment : pageInfo->comments) {
        auto el = ClonePageElement(comment);
        els->Append(el);
    }

    els->Reverse();
}

void FzLinkifyPageText(FzPageInfo* pageInfo, fz_stext_page* stext) {
    if (!pageInfo || !stext) {
        return;
    }

    Rect* coords;
    WCHAR* pageText = FzTextPageToStr(stext, &coords);
    if (!pageText) {
        return;
    }

    LinkRectList* list = LinkifyText(pageText, coords);
    free(pageText);
    // fz_page* page = pageInfo->page;

    for (size_t i = 0; i < list->links.size(); i++) {
        fz_rect bbox = list->coords.at(i);
        bool overlaps = false;
        fz_link* link = pageInfo->links;
        while (link && !overlaps) {
            overlaps = FzRectOverlap(bbox, link->rect) >= 0.25f;
            link = link->next;
        }
        if (overlaps) {
            continue;
        }

        WCHAR* uri = list->links.at(i);
        if (!uri) {
            continue;
        }

        // TODO: those leak on xps
        PageElement* pel = new PageElement();
        pel->kind_ = kindPageElementDest;
        pel->dest = new PageDestination();
        pel->dest->kind = kindDestinationLaunchURL;
        pel->dest->pageNo = 0;
        pel->dest->value = str::Dup(uri);
        pel->value = str::Dup(uri);
        pel->rect = ToRectFl(bbox);
        pageInfo->autoLinks.Append(pel);
    }
    delete list;
    free(coords);
}

void FzFindImagePositions(fz_context* ctx, Vec<FitzImagePos>& images, fz_stext_page* stext) {
    if (!stext) {
        return;
    }
    fz_stext_block* block = stext->first_block;
    fz_image* image;
    while (block) {
        if (block->type != FZ_STEXT_BLOCK_IMAGE) {
            block = block->next;
            continue;
        }
        image = block->u.i.image;
        if (image->colorspace != nullptr) {
            // https://github.com/sumatrapdfreader/sumatrapdf/issues/1480
            // fz_convert_pixmap_samples doesn't handle src without colorspace
            // TODO: this is probably not right
            FitzImagePos img = {block->bbox, block->u.i.transform};
            images.Append(img);
        }
        block = block->next;
    }
}

fz_image* FzFindImageAtIdx(fz_context* ctx, FzPageInfo* pageInfo, int idx) {
    fz_stext_options opts{};
    opts.flags = FZ_STEXT_PRESERVE_IMAGES;
    fz_stext_page* stext = nullptr;
    fz_var(stext);
    fz_try(ctx) {
        stext = fz_new_stext_page_from_page(ctx, pageInfo->page, &opts);
    }
    fz_catch(ctx) {
    }
    if (!stext) {
        return nullptr;
    }
    fz_stext_block* block = stext->first_block;
    while (block) {
        if (block->type != FZ_STEXT_BLOCK_IMAGE) {
            block = block->next;
            continue;
        }
        fz_image* image = block->u.i.image;
        if (image->colorspace != nullptr) {
            // https://github.com/sumatrapdfreader/sumatrapdf/issues/1480
            // fz_convert_pixmap_samples doesn't handle src without colorspace
            // TODO: this is probably not right
            if (idx == 0) {
                // TODO: or maybe get pixmap here
                image = fz_keep_image(ctx, image);
                fz_drop_stext_page(ctx, stext);
                return image;
            }
            idx--;
        }
        block = block->next;
    }
    fz_drop_stext_page(ctx, stext);
    return nullptr;
}

static COLORREF MkColorFromFloat(float r, float g, float b) {
    u8 rb = (u8)(r * 255.0f);
    u8 gb = (u8)(g * 255.0f);
    u8 bb = (u8)(b * 255.0f);
    return MkColor(rb, gb, bb);
}

/*
    n = 1 (grey), 3 (rgb) or 4 (cmyk).
*/
COLORREF ColorRefFromPdfFloat(fz_context* ctx, int n, float color[4]) {
    if (n == 0) {
        return ColorUnset;
    }
    if (n == 1) {
        return MkColorFromFloat(color[0], color[0], color[0]);
    }
    if (n == 3) {
        return MkColorFromFloat(color[0], color[1], color[2]);
    }
    if (n == 4) {
        float rgb[4];
        fz_convert_color(ctx, fz_device_cmyk(ctx), color, fz_device_rgb(ctx), rgb, nullptr, fz_default_color_params);
        return MkColorFromFloat(rgb[0], rgb[1], rgb[2]);
    }
    CrashIf(true);
    return 0;
}
