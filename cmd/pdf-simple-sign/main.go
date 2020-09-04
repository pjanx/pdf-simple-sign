//
// Copyright (c) 2018 - 2020, Přemysl Eric Janouch <p@janouch.name>
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

// pdf-simple-sign is a simple PDF signer.
package main

import (
	"flag"
	"fmt"
	"io/ioutil"
	"os"

	"janouch.name/pdf-simple-sign/pdf"
)

// #include <unistd.h>
import "C"

func isatty(fd uintptr) bool { return C.isatty(C.int(fd)) != 0 }

func die(status int, format string, args ...interface{}) {
	msg := fmt.Sprintf(format+"\n", args...)
	if isatty(os.Stderr.Fd()) {
		msg = "\x1b[0;31m" + msg + "\x1b[m"
	}
	os.Stderr.WriteString(msg)
	os.Exit(status)
}

func usage() {
	die(1, "Usage: %s [-h] [-r RESERVATION] INPUT-FILENAME OUTPUT-FILENAME "+
		"PKCS12-PATH PKCS12-PASS", os.Args[0])
}

var reservation = flag.Int(
	"r", 4096, "signature reservation as a number of bytes")

func main() {
	flag.Usage = usage
	flag.Parse()
	if flag.NArg() != 4 {
		usage()
	}

	inputPath, outputPath := flag.Arg(0), flag.Arg(1)
	doc, err := ioutil.ReadFile(inputPath)
	if err != nil {
		die(1, "%s", err)
	}
	p12, err := ioutil.ReadFile(flag.Arg(2))
	if err != nil {
		die(2, "%s", err)
	}
	key, certs, err := pdf.PKCS12Parse(p12, flag.Arg(3))
	if err != nil {
		die(3, "%s", err)
	}
	if doc, err = pdf.Sign(doc, key, certs, *reservation); err != nil {
		die(4, "error: %s", err)
	}
	if err = ioutil.WriteFile(outputPath, doc, 0666); err != nil {
		die(5, "%s", err)
	}
}
