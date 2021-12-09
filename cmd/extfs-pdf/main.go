//
// Copyright (c) 2021, Přemysl Eric Janouch <p@janouch.name>
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

// extfs-pdf is an external VFS plugin for Midnight Commander.
// More serious image extractors should rewrite this to use pdfimages(1).
package main

import (
	"flag"
	"fmt"
	"os"
	"time"

	"janouch.name/pdf-simple-sign/pdf"
)

func die(status int, format string, args ...interface{}) {
	os.Stderr.WriteString(fmt.Sprintf(format+"\n", args...))
	os.Exit(status)
}

func usage() {
	die(1, "Usage: %s [-h] COMMAND DOCUMENT [ARG...]", os.Args[0])
}

func streamSuffix(o *pdf.Object) string {
	if filter, _ := o.Dict["Filter"]; filter.Kind == pdf.Name {
		switch filter.String {
		case "JBIG2Decode":
			// This is the file extension used by pdfimages(1).
			// This is not a complete JBIG2 standalone file.
			return "jb2e"
		case "JPXDecode":
			return "jp2"
		case "DCTDecode":
			return "jpg"
		default:
			return filter.String
		}
	}
	return "stream"
}

func list(mtime time.Time, updater *pdf.Updater) {
	stamp := mtime.Local().Format("01-02-2006 15:04:05")
	for _, o := range updater.ListIndirect() {
		object, err := updater.Get(o.N, o.Generation)
		size := 0
		if err != nil {
			fmt.Fprintf(os.Stderr, "%s\n", err)
		} else {
			// Accidental transformation, retrieving original data is more work.
			size = len(object.Serialize())
		}
		fmt.Printf("-r--r--r-- 1 0 0 %d %s n%dg%d\n",
			size, stamp, o.N, o.Generation)
		if object.Kind == pdf.Stream {
			fmt.Printf("-r--r--r-- 1 0 0 %d %s n%dg%d.%s\n", len(object.Stream),
				stamp, o.N, o.Generation, streamSuffix(&object))
		}
	}
}

func copyout(updater *pdf.Updater, storedFilename, extractTo string) {
	var (
		n, generation uint
		suffix        string
	)
	m, err := fmt.Sscanf(storedFilename, "n%dg%d%s", &n, &generation, &suffix)
	if m < 2 {
		die(3, "%s: %s", storedFilename, err)
	}

	object, err := updater.Get(n, generation)
	if err != nil {
		die(3, "%s: %s", storedFilename, err)
	}

	content := []byte(object.Serialize())
	if suffix != "" {
		content = object.Stream
	}
	if err = os.WriteFile(extractTo, content, 0666); err != nil {
		die(3, "%s", err)
	}
}

func main() {
	flag.Usage = usage
	flag.Parse()
	if flag.NArg() < 2 {
		usage()
	}

	command, documentPath := flag.Arg(0), flag.Arg(1)
	doc, err := os.ReadFile(documentPath)
	if err != nil {
		die(1, "%s", err)
	}

	mtime := time.UnixMilli(0)
	if info, err := os.Stat(documentPath); err == nil {
		mtime = info.ModTime()
	}

	updater, err := pdf.NewUpdater(doc)
	if err != nil {
		die(2, "%s", err)
	}

	switch command {
	default:
		die(1, "unsupported command: %s", command)
	case "list":
		if flag.NArg() != 2 {
			usage()
		} else {
			list(mtime, updater)
		}
	case "copyout":
		if flag.NArg() != 4 {
			usage()
		} else {
			copyout(updater, flag.Arg(2), flag.Arg(3))
		}
	}
}
