"""Compile the real patched DVI buffer functions in a focused C regression test.

This isolates byte ordering, flush visibility, offset accounting, padding and
error handling. It complements (does not replace) the live engine/converter test.
The functions and padding expression are extracted from --source, not rewritten.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


PREFIX = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <setjmp.h>
typedef unsigned char eight_bits;
typedef struct { FILE *file; } pdf_struct;
typedef pdf_struct *PDF;
static pdf_struct output;
static PDF static_pdf = &output;
static int dvi_buf_size, half_buf, dvi_limit, dvi_ptr, dvi_offset, dvi_gone;
static int dvi_flush_on_shipout, check_dvi_total_pages, total_pages;
static eight_bits *dvi_buf;
enum { eop = 140 };
static jmp_buf error_jump;
static int expect_error, saw_error, fail_write, fail_flush, flush_calls;
static unsigned long verified_pages;
#define CHECK(condition) do { if (!(condition)) { \
  fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); exit(2); \
} } while (0)
static void normal_error(const char *where, const char *message) {
  (void)where; (void)message;
  CHECK(expect_error);
  saw_error = 1;
  longjmp(error_jump, 1);
}
static void print_err(const char *s) { (void)s; }
static void jump_out(void) { normal_error("dvi", "page limit"); }
static int test_fputc(int c, FILE *stream) {
  return fail_write ? EOF : fputc(c, stream);
}
static int test_fflush(FILE *stream) {
  ++flush_calls;
  return fail_flush ? EOF : fflush(stream);
}
static int test_ferror(FILE *stream) {
  return fail_write || ferror(stream);
}
static void write_dvi(int a, int b);
static void dvi_swap(void);
#define fputc test_fputc
#define fflush test_fflush
#define ferror test_ferror
'''

SUFFIX = r'''
#undef fputc
#undef fflush
#undef ferror
static void start_stream(int size) {
  dvi_buf_size = size;
  half_buf = size / 2;
  dvi_buf = (eight_bits *)malloc((size_t)size);
  CHECK(dvi_buf != NULL);
  memset(dvi_buf, 0xDD, (size_t)size);
  dvi_limit = size;
  dvi_ptr = dvi_offset = dvi_gone = total_pages = 0;
  dvi_flush_on_shipout = 1;
  check_dvi_total_pages = 1;
  fail_write = fail_flush = expect_error = saw_error = flush_calls = 0;
  output.file = fopen("buffer-test.bin", "wb");
  CHECK(output.file != NULL);
}
static void stop_stream(void) {
  CHECK(fclose(output.file) == 0);
  free(dvi_buf);
}
static void check_visible(const unsigned char *reference, size_t length, size_t previous) {
  unsigned char actual[4096];
  FILE *reader = fopen("buffer-test.bin", "rb");
  size_t done = previous;
  CHECK(reader != NULL);
  CHECK(fseek(reader, 0, SEEK_END) == 0);
  CHECK(ftell(reader) == (long)length);
  CHECK(fseek(reader, (long)previous, SEEK_SET) == 0);
  while (done < length) {
    size_t want = length - done;
    if (want > sizeof(actual)) want = sizeof(actual);
    CHECK(fread(actual, 1, want, reader) == want);
    CHECK(memcmp(actual, reference + done, want) == 0);
    done += want;
  }
  CHECK(fclose(reader) == 0);
}
static void test_sequences(int size) {
  /* Cross both half-buffer states, including exact boundary and EOP crossings. */
  int lengths[] = {0, 1, 2, 3, 4, size/2-3, size/2-2, size/2-1,
    size/2, size/2+1, size/2+2, size-3, size-2, size-1, size,
    size+1, size+2, size+3, 2*size-3, 2*size-2, 2*size-1,
    2*size, 2*size+1, 2*size+2, 2*size+3, 3*size+1};
  unsigned char *reference = (unsigned char *)malloc((size_t)size * 400 + 10000);
  size_t length = 0;
  unsigned int residues = 0;
  int repeat, index;
  CHECK(reference != NULL);
  start_stream(size);
  for (repeat = 0; repeat < 4; ++repeat) {
    for (index = 0; index < (int)(sizeof(lengths)/sizeof(lengths[0])); ++index) {
      int j, padding;
      size_t previous = length;
      for (j = 0; j < lengths[index]; ++j) {
        unsigned char value = (unsigned char)((length * 37 + index * 11 + repeat) & 255);
        reference[length++] = value;
        dvi_out(value);
      }
      reference[length++] = eop;
      ++total_pages;
      dvi_end_page(static_pdf);
      ++verified_pages;
      CHECK(dvi_offset == (int)length);
      CHECK(dvi_gone == (int)length);
      CHECK(dvi_ptr == 0 && dvi_limit == size);
      CHECK(flush_calls == total_pages);
      residues |= 1u << (length & 3);
      check_visible(reference, length, previous);
      /* Exercise the actual final-padding expression at every logical residue. */
      for (j = 0; j < 8; ++j) {
        dvi_ptr = j;
        padding = actual_padding();
        CHECK(padding >= 4 && padding <= 7);
        CHECK(((dvi_offset + dvi_ptr + padding) & 3) == 0);
      }
      dvi_ptr = 0;
    }
  }
  CHECK(residues == 15);
  check_visible(reference, length, 0);
  stop_stream();
  free(reference);
}
static void test_disabled(void) {
  start_stream(64);
  dvi_flush_on_shipout = 0;
  dvi_out(42);
  total_pages = 1;
  dvi_end_page(static_pdf);
  CHECK(dvi_ptr == 2 && dvi_offset == 0 && dvi_gone == 0);
  CHECK(dvi_buf[0] == 42 && dvi_buf[1] == eop && flush_calls == 0);
  stop_stream();
}
static void test_errors(void) {
  volatile int mode;
  for (mode = 0; mode < 3; ++mode) {
    start_stream(64);
    dvi_out(42);
    total_pages = 1;
    if (mode == 0) fail_flush = 1;
    if (mode == 1) fail_write = 1;
    if (mode == 2) dvi_offset = INT_MAX - 1;
    expect_error = 1;
    if (setjmp(error_jump) == 0) {
      dvi_end_page(static_pdf);
      CHECK(0); /* Each injected error must be reported. */
    }
    CHECK(saw_error);
    if (mode < 2) {
      /* Backend wrapup after an I/O error must not see stale pending bytes. */
      CHECK(dvi_ptr == 0 && dvi_limit == 64);
      CHECK(dvi_offset == 2 && dvi_gone == 2);
    }
    fail_write = fail_flush = 0;
    stop_stream();
  }
}
int main(void) {
  int sizes[] = {8, 16, 64, 800, 16384};
  unsigned int i;
  for (i = 0; i < sizeof(sizes)/sizeof(sizes[0]); ++i)
    test_sequences(sizes[i]);
  test_disabled();
  test_errors();
  printf("PASS: %lu live page flushes, 5 buffer sizes, all offset residues; disabled and I/O/offset error paths\n", verified_pages);
  return 0;
}
'''


def harness(source):
    macro = re.search(r"^#define dvi_out\(A\).*?^} while \(0\)", source, re.M | re.S)
    if not macro:
        raise ValueError("Could not locate dvi_out macro")
    padding_start = source.index("/*tex the number of 223's */")
    padding_end = source.index("while (k > 0)", padding_start)
    padding = source[padding_start:padding_end]
    parts = [PREFIX, macro.group(0), function(source, "static void write_dvi(int a, int b)"),
             function(source, "static void dvi_swap(void)"), function(source, "void dvi_end_page(PDF pdf)"),
             "static int actual_padding(void) { int k;\n" + padding + "return k; }", SUFFIX]
    return "\n".join(parts)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "build/luahbtex-mingw/source/texk/web2c/luatexdir/dvi/dvigen.c")
    parser.add_argument("--cc", default=shutil.which("gcc") or "C:/Qt/Tools/mingw1310_64/bin/gcc.exe")
    parser.add_argument("--output", type=Path, default=ROOT / "build/luatex-flush-buffer-test")
    parser.add_argument("--mutation-checks", action="store_true", help="also prove detection of missing half-buffer, offset, fflush, and padding bugs")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    source = args.source.read_text(encoding="utf-8")
    if "dvi_flush_on_shipout" not in source:
        raise ValueError("Source does not contain the flush patch")
    code = harness(source)
    env = os.environ.copy()
    env["PATH"] = str(Path(args.cc).resolve().parent) + os.pathsep + env.get("PATH", "")

    def run(name, contents, expect_success):
        cfile = output / (name + ".c")
        executable = output / (name + (".exe" if os.name == "nt" else ""))
        cfile.write_text(contents, encoding="utf-8")
        subprocess.run([args.cc, "-std=c99", "-O2", "-Wall", "-Wextra", "-o", str(executable), str(cfile)],
                       cwd=output, env=env, check=True, timeout=30)
        result = subprocess.run([str(executable)], cwd=output, env=env, capture_output=True, text=True, timeout=30)
        (output / (name + ".log")).write_text(result.stdout + result.stderr, encoding="utf-8")
        if (result.returncode == 0) != expect_success:
            raise RuntimeError(f"Unexpected result for {name}: {result.returncode}\n{result.stdout}{result.stderr}")
        print(result.stdout.strip() if expect_success else f"PASS: detected {name}")

    run("actual-source", code, True)
    if args.mutation_checks:
        changes = {
            "missing-older-half": ("if (dvi_limit == half_buf)\n            write_dvi(half_buf, dvi_buf_size - 1);", "if (dvi_limit == half_buf)\n            (void)0;"),
            "stale-offset": ("dvi_offset = end;", "dvi_offset = 0;"),
            "missing-fflush": ("fflush(pdf->file) != 0", "0"),
            "unaligned-padding": ("k = 4 + ((4 - ((dvi_offset + dvi_ptr) & 3)) & 3);", "k = 4 + ((dvi_buf_size - dvi_ptr) % 4);"),
        }
        for name, (old, new) in changes.items():
            if old not in code:
                raise ValueError(f"Mutation anchor missing: {name}")
            run(name, code.replace(old, new), False)


if __name__ == "__main__":
    main()
