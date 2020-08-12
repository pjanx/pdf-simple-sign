// vim: set sw=2 ts=2 sts=2 et tw=100:
//
// pdf-simple-sign: simple PDF signer
//
// Copyright (c) 2017 - 2020, Přemysl Eric Janouch <p@janouch.name>
//
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
// SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
// OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
// CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <cstdarg>

#include <unistd.h>
#include <getopt.h>

#include "pdf-simple-sign.h"

__attribute__((format(printf, 2, 3)))
static void die(int status, const char* format, ...) {
  va_list ap;
  va_start(ap, format);
  if (isatty(fileno(stderr)))
    vfprintf(stderr, ("\x1b[31m" + std::string(format) + "\x1b[0m\n").c_str(), ap);
  else
    vfprintf(stderr, format, ap);
  va_end(ap);
  exit(status);
}

int main(int argc, char* argv[]) {
  auto invocation_name = argv[0];
  auto usage = [=]{
    die(1, "Usage: %s [-h] INPUT-FILENAME OUTPUT-FILENAME PKCS12-PATH PKCS12-PASS",
            invocation_name);
  };

  static struct option opts[] = {
    {"help", no_argument, 0, 'h'},
    {nullptr, 0, 0, 0},
  };

  while (1) {
    int option_index = 0;
    auto c = getopt_long(argc, const_cast<char* const*>(argv),
                         "h", opts, &option_index);
    if (c == -1)
      break;

    switch (c) {
    case 'h': usage(); break;
    default: usage();
    }
  }

  argv += optind;
  argc -= optind;

  if (argc != 4)
    usage();

  const char* input_path  = argv[0];
  const char* output_path = argv[1];

  std::string pdf_document;
  if (auto fp = fopen(input_path, "rb")) {
    int c;
    while ((c = fgetc(fp)) != EOF)
      pdf_document += c;
    if (ferror(fp))
      die(1, "%s: %s", input_path, strerror(errno));
    fclose(fp);
  } else {
    die(1, "%s: %s", input_path, strerror(errno));
  }

  auto err = pdf_simple_sign(pdf_document, argv[2], argv[3]);
  if (!err.empty()) {
    die(2, "Error: %s", err.c_str());
  }

  if (auto fp = fopen(output_path, "wb")) {
    auto written = fwrite(pdf_document.c_str(), pdf_document.size(), 1, fp);
    if (fclose(fp) || written != 1) {
      (void) unlink(output_path);
      die(3, "%s: %s", output_path, strerror(errno));
    }
  } else {
    die(3, "%s: %s", output_path, strerror(errno));
  }
  return 0;
}
