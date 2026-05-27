// DTA parser unit tests — validate text DTA parsing, #include handling,
// #ifdef/#define macros, and the flex lexer holdChar save/restore mechanism.
//
// These test the native DataParser_Native.cpp + DataFlex.c lexer path
// used by dc3-native and dc3-web builds.

#include "test_helpers.h"
#include "obj/Data.h"
#include "obj/DataFile.h"
#include "obj/DataUtl.h"
#include "os/Debug.h"
#include "utl/BufStream.h"

#include <cstring>
#include <cstdio>
#include <string>
#include <sys/stat.h>

// ============================================================================
// Helpers
// ============================================================================

// Parse a DTA string and return the resulting DataArray.
// Caller must Release() the result.
static DataArray *ParseDTA(const char *text) {
    return DataReadString(text);
}

// Write a file to disk for #include testing
static void WriteTestFile(const char *path, const char *content) {
    // Create parent directories
    std::string dir(path);
    for (size_t i = 1; i < dir.size(); i++) {
        if (dir[i] == '/') {
            dir[i] = '\0';
            mkdir(dir.c_str(), 0755);
            dir[i] = '/';
        }
    }
    FILE *f = fopen(path, "w");
    ASSERT_NE(f, nullptr) << "Failed to create test file: " << path;
    fputs(content, f);
    fclose(f);
}

// ============================================================================
// Fixture: Symbol init only (no full engine)
// ============================================================================

class DtaParserTest : public SymbolTestFixture {};

// ============================================================================
// Basic parsing tests
// ============================================================================

TEST_F(DtaParserTest, EmptyString) {
    DataArray *arr = ParseDTA("");
    ASSERT_NE(arr, nullptr);
    EXPECT_EQ(arr->Size(), 0);
    arr->Release();
}

TEST_F(DtaParserTest, SingleInt) {
    DataArray *arr = ParseDTA("42");
    ASSERT_NE(arr, nullptr);
    ASSERT_EQ(arr->Size(), 1);
    EXPECT_EQ(arr->Int(0), 42);
    arr->Release();
}

TEST_F(DtaParserTest, MultipleInts) {
    DataArray *arr = ParseDTA("1 2 3");
    ASSERT_NE(arr, nullptr);
    ASSERT_EQ(arr->Size(), 3);
    EXPECT_EQ(arr->Int(0), 1);
    EXPECT_EQ(arr->Int(1), 2);
    EXPECT_EQ(arr->Int(2), 3);
    arr->Release();
}

TEST_F(DtaParserTest, Float) {
    DataArray *arr = ParseDTA("3.14");
    ASSERT_NE(arr, nullptr);
    ASSERT_EQ(arr->Size(), 1);
    EXPECT_NEAR(arr->Float(0), 3.14f, 0.001f);
    arr->Release();
}

TEST_F(DtaParserTest, Symbol) {
    DataArray *arr = ParseDTA("hello");
    ASSERT_NE(arr, nullptr);
    ASSERT_EQ(arr->Size(), 1);
    EXPECT_EQ(arr->Node(0).Type(), kDataSymbol);
    EXPECT_STREQ(arr->Sym(0).Str(), "hello");
    arr->Release();
}

TEST_F(DtaParserTest, QuotedString) {
    DataArray *arr = ParseDTA("\"hello world\"");
    ASSERT_NE(arr, nullptr);
    ASSERT_EQ(arr->Size(), 1);
    EXPECT_EQ(arr->Node(0).Type(), kDataString);
    EXPECT_STREQ(arr->Str(0), "hello world");
    arr->Release();
}

TEST_F(DtaParserTest, HexLiteral) {
    DataArray *arr = ParseDTA("0xFF");
    ASSERT_NE(arr, nullptr);
    ASSERT_EQ(arr->Size(), 1);
    EXPECT_EQ(arr->Int(0), 255);
    arr->Release();
}

TEST_F(DtaParserTest, NestedArray) {
    DataArray *arr = ParseDTA("(foo 1 2)");
    ASSERT_NE(arr, nullptr);
    ASSERT_EQ(arr->Size(), 1);
    DataArray *inner = arr->Array(0);
    ASSERT_EQ(inner->Size(), 3);
    EXPECT_STREQ(inner->Sym(0).Str(), "foo");
    EXPECT_EQ(inner->Int(1), 1);
    EXPECT_EQ(inner->Int(2), 2);
    arr->Release();
}

TEST_F(DtaParserTest, MultipleNestedArrays) {
    DataArray *arr = ParseDTA("(a 1) (b 2) (c 3)");
    ASSERT_NE(arr, nullptr);
    ASSERT_EQ(arr->Size(), 3);
    EXPECT_STREQ(arr->Array(0)->Sym(0).Str(), "a");
    EXPECT_STREQ(arr->Array(1)->Sym(0).Str(), "b");
    EXPECT_STREQ(arr->Array(2)->Sym(0).Str(), "c");
    arr->Release();
}

TEST_F(DtaParserTest, DeeplyNested) {
    DataArray *arr = ParseDTA("((1 (2 (3))))");
    ASSERT_NE(arr, nullptr);
    ASSERT_EQ(arr->Size(), 1);
    DataArray *l1 = arr->Array(0);
    ASSERT_EQ(l1->Size(), 1);
    DataArray *l2 = l1->Array(0);
    ASSERT_EQ(l2->Size(), 2);
    EXPECT_EQ(l2->Int(0), 1);
    DataArray *l3 = l2->Array(1);
    ASSERT_EQ(l3->Size(), 2);
    EXPECT_EQ(l3->Int(0), 2);
    DataArray *l4 = l3->Array(1);
    ASSERT_EQ(l4->Size(), 1);
    EXPECT_EQ(l4->Int(0), 3);
    arr->Release();
}

// ============================================================================
// #define / macro tests
// ============================================================================

TEST_F(DtaParserTest, DefineAndUse) {
    DataArray *arr = ParseDTA("#define FOO (42)\nFOO");
    ASSERT_NE(arr, nullptr);
    // FOO macro expands inline: the macro value (42) is inserted
    // The define itself produces no output, FOO expands to 42
    ASSERT_GE(arr->Size(), 1);
    EXPECT_EQ(arr->Int(0), 42);
    arr->Release();
    // Clean up macro
    DataSetMacro(Symbol("FOO"), nullptr);
}

TEST_F(DtaParserTest, DefineArray) {
    DataArray *arr = ParseDTA("#define BAR (1 2 3)\nBAR");
    ASSERT_NE(arr, nullptr);
    // BAR expands to 1 2 3 inline
    ASSERT_GE(arr->Size(), 3);
    EXPECT_EQ(arr->Int(0), 1);
    EXPECT_EQ(arr->Int(1), 2);
    EXPECT_EQ(arr->Int(2), 3);
    arr->Release();
    DataSetMacro(Symbol("BAR"), nullptr);
}

// ============================================================================
// #ifdef / #ifndef / #else / #endif tests
// ============================================================================

TEST_F(DtaParserTest, IfdefDefined) {
    DataSetMacro(Symbol("TEST_MACRO"), DataReadString("(1)"));
    DataArray *arr = ParseDTA("#ifdef TEST_MACRO\n42\n#endif");
    ASSERT_NE(arr, nullptr);
    ASSERT_EQ(arr->Size(), 1);
    EXPECT_EQ(arr->Int(0), 42);
    arr->Release();
    DataSetMacro(Symbol("TEST_MACRO"), nullptr);
}

TEST_F(DtaParserTest, IfdefUndefined) {
    DataArray *arr = ParseDTA("#ifdef NONEXISTENT_MACRO\n42\n#endif");
    ASSERT_NE(arr, nullptr);
    EXPECT_EQ(arr->Size(), 0);
    arr->Release();
}

TEST_F(DtaParserTest, IfdefElse) {
    DataArray *arr = ParseDTA("#ifdef NONEXISTENT_MACRO\n42\n#else\n99\n#endif");
    ASSERT_NE(arr, nullptr);
    ASSERT_EQ(arr->Size(), 1);
    EXPECT_EQ(arr->Int(0), 99);
    arr->Release();
}

TEST_F(DtaParserTest, IfndefUndefined) {
    DataArray *arr = ParseDTA("#ifndef NONEXISTENT_MACRO\n42\n#endif");
    ASSERT_NE(arr, nullptr);
    ASSERT_EQ(arr->Size(), 1);
    EXPECT_EQ(arr->Int(0), 42);
    arr->Release();
}

TEST_F(DtaParserTest, NestedConditionals) {
    DataSetMacro(Symbol("OUTER"), DataReadString("(1)"));
    DataArray *arr = ParseDTA(
        "#ifdef OUTER\n"
        "  1\n"
        "  #ifdef NONEXISTENT\n"
        "    2\n"
        "  #else\n"
        "    3\n"
        "  #endif\n"
        "#endif\n"
    );
    ASSERT_NE(arr, nullptr);
    ASSERT_EQ(arr->Size(), 2);
    EXPECT_EQ(arr->Int(0), 1);
    EXPECT_EQ(arr->Int(1), 3);
    arr->Release();
    DataSetMacro(Symbol("OUTER"), nullptr);
}

// ============================================================================
// #include tests — requires writing temporary files
// ============================================================================

class DtaIncludeTest : public SymbolTestFixture {
protected:
    void SetUp() override {
        // Create temp directory for test files
        mTestDir = "/tmp/claude-1000/dta_test";
        mkdir("/tmp/claude-1000", 0755);
        mkdir(mTestDir.c_str(), 0755);
    }

    void TearDown() override {
        // Clean up test files
        std::string cmd = "rm -rf " + mTestDir;
        system(cmd.c_str());
    }

    std::string mTestDir;
};

TEST_F(DtaIncludeTest, SimpleInclude) {
    // Create included file
    std::string inclPath = mTestDir + "/included.dta";
    WriteTestFile(inclPath.c_str(), "99\n");

    // Create main file that includes it
    std::string mainPath = mTestDir + "/main.dta";
    std::string mainContent = "(foo 1)\n#include included.dta\n(bar 2)\n";
    WriteTestFile(mainPath.c_str(), mainContent.c_str());

    DataArray *arr = DataReadFile(mainPath.c_str(), true);
    ASSERT_NE(arr, nullptr);
    // Should have: (foo 1), 99, (bar 2)
    ASSERT_GE(arr->Size(), 3);
    EXPECT_STREQ(arr->Array(0)->Sym(0).Str(), "foo");
    EXPECT_EQ(arr->Int(1), 99);
    EXPECT_STREQ(arr->Array(2)->Sym(0).Str(), "bar");
    arr->Release();
}

TEST_F(DtaIncludeTest, IncludeWithTrailingParen) {
    // This is the critical synth.dta pattern:
    //   (scenes #include metamusic_scenes.dta)
    // The ')' immediately after the filename closes the parent array.
    // The lexer reads ')' as a lookahead byte (yy_hold_char) before
    // returning the SYMBOL token for the filename. ReadEmbeddedFile
    // must save and restore this holdChar across the include.

    std::string inclPath = mTestDir + "/inner.dta";
    WriteTestFile(inclPath.c_str(), "a b c\n");

    std::string mainPath = mTestDir + "/outer.dta";
    std::string mainContent = "(scenes #include inner.dta)\n(after_include 42)\n";
    WriteTestFile(mainPath.c_str(), mainContent.c_str());

    DataArray *arr = DataReadFile(mainPath.c_str(), true);
    ASSERT_NE(arr, nullptr);
    // Should have: (scenes a b c), (after_include 42)
    ASSERT_GE(arr->Size(), 2) << "Expected at least 2 top-level elements";

    DataArray *scenes = arr->Array(0);
    ASSERT_GE(scenes->Size(), 4) << "scenes array should have: scenes a b c";
    EXPECT_STREQ(scenes->Sym(0).Str(), "scenes");
    EXPECT_STREQ(scenes->Sym(1).Str(), "a");
    EXPECT_STREQ(scenes->Sym(2).Str(), "b");
    EXPECT_STREQ(scenes->Sym(3).Str(), "c");

    DataArray *after = arr->Array(1);
    EXPECT_STREQ(after->Sym(0).Str(), "after_include");
    EXPECT_EQ(after->Int(1), 42);
    arr->Release();
}

TEST_F(DtaIncludeTest, NestedIncludes) {
    // A includes B which includes C
    std::string cPath = mTestDir + "/c.dta";
    WriteTestFile(cPath.c_str(), "from_c\n");

    std::string bPath = mTestDir + "/b.dta";
    WriteTestFile(bPath.c_str(), "from_b\n#include c.dta\n");

    std::string aPath = mTestDir + "/a.dta";
    WriteTestFile(aPath.c_str(), "(top #include b.dta)\n");

    DataArray *arr = DataReadFile(aPath.c_str(), true);
    ASSERT_NE(arr, nullptr);
    ASSERT_GE(arr->Size(), 1);
    DataArray *top = arr->Array(0);
    // Should have: top from_b from_c
    ASSERT_GE(top->Size(), 3);
    EXPECT_STREQ(top->Sym(0).Str(), "top");
    EXPECT_STREQ(top->Sym(1).Str(), "from_b");
    EXPECT_STREQ(top->Sym(2).Str(), "from_c");
    arr->Release();
}

TEST_F(DtaIncludeTest, IncludeWithIfdef) {
    // Include a file that uses #ifdef
    std::string inclPath = mTestDir + "/conditional.dta";
    WriteTestFile(inclPath.c_str(),
        "#ifdef COND_TEST\n"
        "  yes\n"
        "#else\n"
        "  no\n"
        "#endif\n"
    );

    DataSetMacro(Symbol("COND_TEST"), DataReadString("(1)"));

    std::string mainPath = mTestDir + "/main_cond.dta";
    WriteTestFile(mainPath.c_str(), "#include conditional.dta\n");

    DataArray *arr = DataReadFile(mainPath.c_str(), true);
    ASSERT_NE(arr, nullptr);
    ASSERT_GE(arr->Size(), 1);
    EXPECT_STREQ(arr->Sym(0).Str(), "yes");
    arr->Release();
    DataSetMacro(Symbol("COND_TEST"), nullptr);
}

TEST_F(DtaIncludeTest, MultipleIncludesInSequence) {
    // Multiple #include directives in the same file
    std::string f1 = mTestDir + "/f1.dta";
    WriteTestFile(f1.c_str(), "1\n");
    std::string f2 = mTestDir + "/f2.dta";
    WriteTestFile(f2.c_str(), "2\n");
    std::string f3 = mTestDir + "/f3.dta";
    WriteTestFile(f3.c_str(), "3\n");

    std::string mainPath = mTestDir + "/multi.dta";
    WriteTestFile(mainPath.c_str(),
        "#include f1.dta\n"
        "#include f2.dta\n"
        "#include f3.dta\n"
    );

    DataArray *arr = DataReadFile(mainPath.c_str(), true);
    ASSERT_NE(arr, nullptr);
    ASSERT_EQ(arr->Size(), 3);
    EXPECT_EQ(arr->Int(0), 1);
    EXPECT_EQ(arr->Int(1), 2);
    EXPECT_EQ(arr->Int(2), 3);
    arr->Release();
}
