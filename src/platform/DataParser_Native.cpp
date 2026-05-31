// DataParser_Native.cpp — DTA text parser for native/web builds
// Ported from RB3 decomp (same Milo engine). These functions are undecompiled
// in DC3 but are essential for parsing .dta config files.

#include "obj/Data.h"
#include "obj/DataFile.h"
#include "obj/DataUtl.h"
#include "os/Debug.h"
#include "utl/BinStream.h"
#include "utl/MemMgr.h"

// Flex lexer interface
#include "obj/DataFlex.h"

#include <cstdlib>
#include <cstring>
#include <list>

// ============================================================================
// Globals shared with DataFile.cpp (defined there as externs or commented out)
// These must be defined exactly once — DataFile.cpp has them commented out,
// so we define them here for the native build.
// ============================================================================

// Already defined in DataFile.cpp:
//   gNode, gFile, gBinStream, gDataLine, gConditional, gReadFiles, gReadingFile
// We need to define the ones that are commented out in DC3's DataFile.cpp:

// On macOS, the linker doesn't support --allow-multiple-definition,
// so mark these as weak to let DataFile.cpp's definitions win.
#ifdef __APPLE__
#define WEAK_SYMBOL __attribute__((weak))
#else
#define WEAK_SYMBOL
#endif

WEAK_SYMBOL DataArray *gArray;
WEAK_SYMBOL int gOpenArray = 0; // kDataTokenFinished = 0

WEAK_SYMBOL bool gCachingFile;

// ============================================================================
// Conditional info for #ifdef/#ifndef tracking
// ============================================================================

struct ConditionalInfo {
    union {
        bool condition;
        int _;
    };
    Symbol file;
    int line;
};

// DC3's DataFile.cpp declares gConditional as std::list<bool>.
// RB3 uses std::list<ConditionalInfo>. For the native port we use our own
// local conditional tracking, separate from the decomp's gConditional.
static std::list<ConditionalInfo> sConditional;

// ============================================================================
// Forward declarations
// ============================================================================

WEAK_SYMBOL void PushBack(const DataNode &n);
static bool Defined();
WEAK_SYMBOL bool ParseNode();

// Extern globals from DataFile.cpp
extern int gNode;
extern Symbol gFile;
extern BinStream *gBinStream;
extern int gDataLine; // line counter (DataFile.cpp: `int gDataLine = 0;`); was wrongly DataType

// ============================================================================
// DataInput — feeds data from BinStream to the flex lexer
// On native builds, this replaces the stub in engine_stubs_generated.cpp.
// We use __attribute__((used)) to ensure the linker picks this over the stub.
// ============================================================================

extern "C" WEAK_SYMBOL int DataInput(void *v, int x) {
    if (!gBinStream) {
        return 0;
    }
    if (gBinStream->Fail()) {
        return 0;
    } else if (gBinStream->Eof()) {
        return 0;
    } else {
        gBinStream->Read(v, x);
        return x;
    }
}

// ============================================================================
// PushBack — add a node to the current parse array
// ============================================================================

WEAK_SYMBOL void PushBack(const DataNode &n) {
    if (gNode == gArray->Size()) {
        if (gNode >= 0x7FFF) {
            MILO_FAIL(
                "%s(%d): array size > max %d lines",
                gArray->File(), gArray->Line(), 0x7FFF
            );
        }
        int x = gNode << 1;
        gArray->Resize(x <= 0x7FFF ? x : 0x7FFF);
    }
    gArray->Node(gNode++) = n;
}

// ============================================================================
// Defined — check if all conditional blocks are true
// ============================================================================

static bool Defined() {
    for (std::list<ConditionalInfo>::iterator it = sConditional.begin();
         it != sConditional.end(); it++) {
        if (!it->condition)
            return false;
    }
    return true;
}

// ============================================================================
// ParseNode — parse a single token from the lexer
// ============================================================================

WEAK_SYMBOL bool ParseNode() {
    int token = yylex();

    if (!Defined() && token != kDataTokenIfdef && token != kDataTokenIfndef
        && token != kDataTokenElse && token != kDataTokenEndif) {
        return true;
    }

    // BOM check at start of file
    char bom[3] = { (char)0xEF, (char)0xBB, (char)0xBF };
    if (gNode == 0 && strncmp(yytext, bom, 3) == 0) {
        if (yyleng > 3)
            MILO_FAIL(
                "%s starts with a ByteOrderMark, put a line return at the top of its file",
                gFile
            );
        else
            return true;
    }

    if (token == kDataTokenFinished) {
        switch (gOpenArray) {
        case kDataTokenArrayOpen:
            MILO_FAIL("Array closed incorrectly (file %s, line %d)", gFile, gDataLine);
            break;
        case kDataTokenCommandOpen:
            MILO_FAIL("Command closed incorrectly (file %s, line %d)", gFile, gDataLine);
            break;
        case kDataTokenPropertyOpen:
            MILO_FAIL("Property closed incorrectly (file %s, line %d)", gFile, gDataLine);
            break;
        default:
            break;
        }
        return false;
    } else if (token == kDataTokenArrayClose) {
        switch (gOpenArray) {
        case kDataTokenFinished:
            MILO_FAIL("File %s ends with open array", gFile);
            break;
        case kDataTokenCommandOpen:
            MILO_FAIL("Command closed incorrectly (file %s, line %d)", gFile, gDataLine);
            break;
        case kDataTokenPropertyOpen:
            MILO_FAIL("Property closed incorrectly (file %s, line %d)", gFile, gDataLine);
            break;
        default:
            break;
        }
        return false;
    } else if (token == kDataTokenPropertyClose) {
        switch (gOpenArray) {
        case kDataTokenFinished:
            MILO_FAIL("File %s ends with open array", gFile);
            break;
        case kDataTokenArrayOpen:
            MILO_FAIL("Array closed incorrectly (file %s, line %d)", gFile, gDataLine);
            break;
        case kDataTokenCommandOpen:
            MILO_FAIL("Command closed incorrectly (file %s, line %d)", gFile, gDataLine);
            break;
        default:
            break;
        }
        return false;
    } else if (token == kDataTokenCommandClose) {
        switch (gOpenArray) {
        case kDataTokenFinished:
            MILO_FAIL("File %s ends with open array", gFile);
            break;
        case kDataTokenArrayOpen:
            MILO_FAIL("Array closed incorrectly (file %s, line %d)", gFile, gDataLine);
            break;
        case kDataTokenPropertyOpen:
            MILO_FAIL("Property closed incorrectly (file %s, line %d)", gFile, gDataLine);
            break;
        default:
            break;
        }
        return false;
    }

    if (token == kDataTokenMerge) {
        if (yylex() != kDataTokenSymbol) {
            MILO_FAIL(
                "DataReadFile: merging a non-symbol (file %s, line %d)", gFile, gDataLine
            );
        }
        if (gCachingFile) {
            PushBack(DataNode(kDataMerge, Symbol(yytext).Str()));
        } else {
            bool usingEmbedded = false;
            DataArray *fileArr = DataGetMacro(yytext);
            if (!fileArr) {
                fileArr = ReadEmbeddedFile(yytext, true);
                usingEmbedded = true;
            }
            if (fileArr && fileArr->Size() == 0) {
                MILO_FAIL("Empty merge file (possibly a re-included file): %s", yytext);
            }
            gArray->Resize(gNode);
            DataMergeTags(gArray, fileArr);
            gNode = gArray->Size();
            if (usingEmbedded) {
                fileArr->Release();
            }
        }
        return true;
    } else if (token == kDataTokenInclude || token == kDataTokenIncludeOptional) {
        bool required = token == kDataTokenInclude;
        if (yylex() != kDataTokenSymbol) {
            MILO_FAIL(
                "DataReadFile: including a non-symbol (file %s, line %d)",
                gFile, gDataLine
            );
        }
        if (gCachingFile) {
            PushBack(DataNode(kDataInclude, Symbol(yytext).Str()));
        } else {
            DataArray *fileArr = ReadEmbeddedFile(yytext, required);
            if (fileArr) {
                for (int i = 0; i < fileArr->Size(); i++) {
                    PushBack(fileArr->Node(i));
                }
                fileArr->Release();
            }
        }
        return true;
    }

    switch (token) {
    case kDataTokenIfdef:
    case kDataTokenIfndef: {
        bool positive = token == kDataTokenIfdef;

        int symToken = yylex();
        bool isSymbol = symToken == kDataTokenSymbol || symToken == kDataTokenQuotedSymbol;
        if (!isSymbol) {
            MILO_FAIL(
                "DataReadFile: not macro symbol (file %s, line %d)", gFile, gDataLine
            );
        }

        char *text;
        if (symToken == kDataTokenQuotedSymbol) {
            yytext[yyleng - 1] = '\0';
            text = yytext + 1;
        } else {
            text = yytext;
        }

        Symbol macro(text);
        if (positive) {
            if (gCachingFile) {
                PushBack(DataNode(kDataIfdef, macro.Str()));
            } else {
                bool defined = DataGetMacro(macro) != 0;
                ConditionalInfo info;
                info.condition = defined;
                info.file = gFile;
                info.line = gDataLine;
                sConditional.push_back(info);
            }
        } else {
            if (gCachingFile) {
                PushBack(DataNode(kDataIfndef, macro.Str()));
            } else {
                bool ndefined = DataGetMacro(macro) == 0;
                ConditionalInfo info;
                info.condition = ndefined;
                info.file = gFile;
                info.line = gDataLine;
                sConditional.push_back(info);
            }
        }
        return true;
    }

    case kDataTokenElse: {
        if (gCachingFile) {
            PushBack(DataNode(kDataElse, 0));
        } else {
            if (sConditional.empty()) {
                MILO_FAIL(
                    "DataReadFile: #else not in conditional (file %s, line %d)",
                    gFile, gDataLine
                );
            }
            sConditional.back().condition = !sConditional.back().condition;
        }
        return true;
    }

    case kDataTokenEndif: {
        if (gCachingFile) {
            PushBack(DataNode(kDataEndif, 0));
        } else {
            if (sConditional.empty()) {
                MILO_FAIL(
                    "DataReadFile: #endif not in conditional (file %s, line %d)",
                    gFile, gDataLine
                );
            }
            sConditional.pop_back();
        }
        return true;
    }

    case kDataTokenAutorun: {
        int cmdToken = yylex();
        if (cmdToken != kDataTokenCommandOpen) {
            MILO_FAIL("DataReadFile: not command (file %s, line %d)", gFile, gDataLine);
        }

        int openArray = gOpenArray;
        gOpenArray = cmdToken;
        DataArray *array = ParseArray();
        gOpenArray = openArray;

        DataNode node(array, kDataCommand);
        if (gCachingFile) {
            PushBack(DataNode(kDataAutorun, 0));
            PushBack(node);
        } else {
            node.Command(array)->Execute();
        }

        array->Release();
        return true;
    }

    case kDataTokenDefine: {
        if (yylex() != kDataTokenSymbol) {
            MILO_FAIL("DataReadFile: not symbol (file %s, line %d)", gFile, gDataLine);
        }

        Symbol macro(yytext);

        int cmdToken = yylex();
        if (cmdToken != kDataTokenArrayOpen) {
            MILO_FAIL("DataReadFile: not array (file %s, line %d)", gFile, gDataLine);
        }

        int openArray = gOpenArray;
        gOpenArray = cmdToken;
        DataArray *array = ParseArray();
        gOpenArray = openArray;

        if (gCachingFile) {
            PushBack(DataNode(kDataDefine, macro.Str()));
            PushBack(DataNode(array, kDataArray));
        } else {
            DataSetMacro(macro, array);
        }

        array->Release();
        return true;
    }

    case kDataTokenUndef: {
        if (yylex() != kDataTokenSymbol) {
            MILO_FAIL("DataReadFile: not symbol (file %s, line %d)", gFile, gDataLine);
        }

        Symbol macro(yytext);
        if (gCachingFile) {
            PushBack(DataNode(kDataUndef, macro.Str()));
        } else {
            DataSetMacro(macro, nullptr);
        }
        return true;
    }

    case kDataTokenArrayOpen:
    case kDataTokenPropertyOpen:
    case kDataTokenCommandOpen: {
        int openArray = gOpenArray;
        gOpenArray = token;
        DataArray *array = ParseArray();
        gOpenArray = openArray;

        DataType type;
        if (token == kDataTokenArrayOpen) {
            type = kDataArray;
        } else if (token == kDataTokenCommandOpen) {
            type = kDataCommand;
        } else {
            type = kDataProperty;
        }

        PushBack(DataNode(array, type));
        array->Release();
        return true;
    }

    case kDataTokenVar: {
        PushBack(&DataVariable(yytext + 1));
        return true;
    }

    case kDataTokenUnhandled: {
        PushBack(DataNode(kDataUnhandled, 0));
        return true;
    }

    case kDataTokenInt: {
        PushBack(atoi(yytext));
        return true;
    }

    case kDataTokenHex: {
        int i = 0;
        int base = 1;
        for (char *c = yytext + strlen(yytext) - 1; *c != 'x'; --c, base <<= 4) {
            if (*c >= 'a') {
                i += (*c - 'a' + 10) * base;
            } else if (*c >= 'A') {
                i += (*c - 'A' + 10) * base;
            } else {
                i += (*c - '0') * base;
            }
        }
        PushBack(i);
        return true;
    }

    case kDataTokenFloat: {
        PushBack((float)atof(yytext));
        return true;
    }

    default:
        break;
    }

    if (token == kDataTokenSymbol || token == kDataTokenQuotedSymbol) {
        char *text;
        if (token == kDataTokenQuotedSymbol) {
            yytext[yyleng - 1] = '\0';
            text = yytext + 1;
        } else {
            text = yytext;
        }

        Symbol sym(text);
        DataArray *macro = DataGetMacro(sym);
        bool b = macro && !gCachingFile;
        if (b) {
            for (int i = 0; i < macro->Size(); i++) {
                PushBack(macro->Node(i));
            }
        } else {
            PushBack(sym);
        }
        return true;
    } else if (token == kDataTokenString) {
        yytext[yyleng - 1] = '\0';
        char *text = yytext + 1;

        for (char *c = text; *c != '\0'; c++) {
            bool escaped = false;
            if (*c == '\\') {
                if (c[1] == 'n') {
                    *c = '\n';
                    escaped = true;
                } else if (c[1] == 'q') {
                    *c = '\"';
                    escaped = true;
                }
            } else if (*c == '\n') {
                gDataLine = (DataType)((int)gDataLine + 1);
            }

            if (escaped) {
                for (char *d = c + 1; *d != '\0'; d++) {
                    *d = *(d + 1);
                }
            }
        }

        PushBack(text);
        return true;
    } else {
        MILO_FAIL(
            "DataReadFile: Unrecognized token %d (file %s, line %d)",
            token, gFile, gDataLine
        );
        return false;
    }
}

// ============================================================================
// ParseArray — parse a complete DTA array (top-level or nested)
// ============================================================================

WEAK_SYMBOL DataArray *ParseArray() {
    DataArray *sav = gArray;
    int nod = gNode;
    DataArray *da = new DataArray(16);
    gArray = da;
    da->SetFileLine(gFile, gDataLine);
    gNode = 0;
    do
        ;
    while (ParseNode());
    gArray->Resize(gNode);
    da = gArray;
    gArray = sav;
    gNode = nod;
    return da;
}
