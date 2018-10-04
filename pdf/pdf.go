//
// Copyright (c) 2018, Přemysl Janouch <p@janouch.name>
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

// Package pdf signs PDF documents and provides some processing utilities.
package pdf

import (
	"bytes"
	"encoding/hex"
	"errors"
	"fmt"
	"math"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"

	"crypto"
	"crypto/ecdsa"
	"crypto/rsa"
	"crypto/x509"
	"go.mozilla.org/pkcs7"
	"golang.org/x/crypto/pkcs12"
)

type ObjectKind int

const (
	End ObjectKind = iota
	NL
	Comment
	Nil
	Bool
	Numeric
	Keyword
	Name
	String

	// simple tokens
	BArray
	EArray
	BDict
	EDict

	// higher-level objects
	Array
	Dict
	Indirect
	Reference
)

// Object is a PDF token/object thingy.  Objects may be composed either from
// one or a sequence of tokens. The PDF Reference doesn't actually speak
// of tokens.
//
// TODO(p): We probably want constructors like NewString, NewBool, NewArray, ...
type Object struct {
	Kind ObjectKind

	// End (error message), Comment/Keyword/Name/String
	String string
	// Bool, Numeric
	Number float64
	// Array, Indirect
	Array []Object
	// Dict, in the future also Stream
	Dict map[string]Object
	// Indirect, Reference
	N, Generation uint
}

// IsInteger checks if the PDF object is an integer number.
func (o *Object) IsInteger() bool {
	_, f := math.Modf(o.Number)
	return o.Kind == Numeric && f == 0
}

// IsUint checks if the PDF object is an integer number that fits into a uint.
func (o *Object) IsUint() bool {
	return o.IsInteger() && o.Number >= 0 && o.Number <= float64(^uint(0))
}

// -----------------------------------------------------------------------------

const (
	octAlphabet = "01234567"
	decAlphabet = "0123456789"
	hexAlphabet = "0123456789abcdefABCDEF"
	whitespace  = "\t\n\f\r "
	delimiters  = "()<>[]{}/%"
)

// Lexer is a basic lexical analyser for the Portable Document Format,
// giving limited error information.
type Lexer struct {
	p []byte // input buffer
}

func (lex *Lexer) read() (byte, bool) {
	if len(lex.p) > 0 {
		ch := lex.p[0]
		lex.p = lex.p[1:]
		return ch, true
	}
	return 0, false
}

func (lex *Lexer) peek() (byte, bool) {
	if len(lex.p) > 0 {
		return lex.p[0], true
	}
	return 0, false
}

func (lex *Lexer) eatNewline(ch byte) bool {
	if ch == '\r' {
		if ch, _ := lex.peek(); ch == '\n' {
			lex.read()
		}
		return true
	}
	return ch == '\n'
}

func (lex *Lexer) unescape(ch byte) byte {
	switch ch {
	case 'n':
		return '\n'
	case 'r':
		return '\r'
	case 't':
		return '\t'
	case 'b':
		return '\b'
	case 'f':
		return '\f'
	}
	if strings.IndexByte(octAlphabet, ch) >= 0 {
		octal := []byte{ch}
		lex.read()
		if ch, _ := lex.peek(); strings.IndexByte(octAlphabet, ch) >= 0 {
			octal = append(octal, ch)
			lex.read()
		}
		if ch, _ := lex.peek(); strings.IndexByte(octAlphabet, ch) >= 0 {
			octal = append(octal, ch)
			lex.read()
		}
		u, _ := strconv.ParseUint(string(octal), 8, 8)
		return byte(u)
	}
	return ch
}

func (lex *Lexer) string() Object {
	var value []byte
	parens := 1
	for {
		ch, ok := lex.read()
		if !ok {
			return Object{Kind: End, String: "unexpected end of string"}
		}
		if lex.eatNewline(ch) {
			ch = '\n'
		} else if ch == '(' {
			parens++
		} else if ch == ')' {
			if parens--; parens == 0 {
				break
			}
		} else if ch == '\\' {
			if ch, ok = lex.read(); !ok {
				return Object{Kind: End, String: "unexpected end of string"}
			} else if lex.eatNewline(ch) {
				continue
			} else {
				ch = lex.unescape(ch)
			}
		}
		value = append(value, ch)
	}
	return Object{Kind: String, String: string(value)}
}

func (lex *Lexer) stringHex() Object {
	var value, buf []byte
	for {
		ch, ok := lex.read()
		if !ok {
			return Object{Kind: End, String: "unexpected end of hex string"}
		} else if ch == '>' {
			break
		} else if strings.IndexByte(hexAlphabet, ch) < 0 {
			return Object{Kind: End, String: "invalid hex string"}
		} else if buf = append(buf, ch); len(buf) == 2 {
			u, _ := strconv.ParseUint(string(buf), 16, 8)
			value = append(value, byte(u))
			buf = nil
		}
	}
	if len(buf) > 0 {
		u, _ := strconv.ParseUint(string(buf)+"0", 16, 8)
		value = append(value, byte(u))
	}
	return Object{Kind: String, String: string(value)}
}

func (lex *Lexer) name() Object {
	var value []byte
	for {
		ch, ok := lex.peek()
		if !ok || strings.IndexByte(whitespace+delimiters, ch) >= 0 {
			break
		}
		lex.read()
		if ch == '#' {
			var hexa []byte
			if ch, _ := lex.peek(); strings.IndexByte(hexAlphabet, ch) >= 0 {
				hexa = append(hexa, ch)
				lex.read()
			}
			if ch, _ := lex.peek(); strings.IndexByte(hexAlphabet, ch) >= 0 {
				hexa = append(hexa, ch)
				lex.read()
			}
			if len(hexa) != 2 {
				return Object{Kind: End, String: "invalid name hexa escape"}
			}
			u, _ := strconv.ParseUint(string(value), 16, 8)
			ch = byte(u)
		}
		value = append(value, ch)
	}
	if len(value) == 0 {
		return Object{Kind: End, String: "unexpected end of name"}
	}
	return Object{Kind: Name, String: string(value)}
}

func (lex *Lexer) comment() Object {
	var value []byte
	for {
		ch, ok := lex.peek()
		if !ok || ch == '\r' || ch == '\n' {
			break
		}
		value = append(value, ch)
		lex.read()
	}
	return Object{Kind: Comment, String: string(value)}
}

// XXX: Maybe invalid numbers should rather be interpreted as keywords.
func (lex *Lexer) number() Object {
	var value []byte
	ch, ok := lex.peek()
	if ch == '-' {
		value = append(value, ch)
		lex.read()
	}
	real, digits := false, false
	for {
		ch, ok = lex.peek()
		if !ok {
			break
		} else if strings.IndexByte(decAlphabet, ch) >= 0 {
			digits = true
		} else if ch == '.' && !real {
			real = true
		} else {
			break
		}
		value = append(value, ch)
		lex.read()
	}
	if !digits {
		return Object{Kind: End, String: "invalid number"}
	}
	f, _ := strconv.ParseFloat(string(value), 64)
	return Object{Kind: Numeric, Number: f}
}

func (lex *Lexer) Next() Object {
	ch, ok := lex.peek()
	if !ok {
		return Object{Kind: End}
	}
	if strings.IndexByte("-0123456789.", ch) >= 0 {
		return lex.number()
	}

	// {} end up being keywords, we might want to error out on those.
	var value []byte
	for {
		ch, ok := lex.peek()
		if !ok || strings.IndexByte(whitespace+delimiters, ch) >= 0 {
			break
		}
		value = append(value, ch)
		lex.read()
	}
	switch v := string(value); v {
	case "":
	case "null":
		return Object{Kind: Nil}
	case "true":
		return Object{Kind: Bool, Number: 1}
	case "false":
		return Object{Kind: Bool, Number: 0}
	default:
		return Object{Kind: Keyword, String: v}
	}

	switch ch, _ := lex.read(); ch {
	case '/':
		return lex.name()
	case '%':
		return lex.comment()
	case '(':
		return lex.string()
	case '[':
		return Object{Kind: BArray}
	case ']':
		return Object{Kind: EArray}
	case '<':
		if ch, _ := lex.peek(); ch == '<' {
			lex.read()
			return Object{Kind: BDict}
		}
		return lex.stringHex()
	case '>':
		if ch, _ := lex.peek(); ch == '>' {
			lex.read()
			return Object{Kind: EDict}
		}
		return Object{Kind: End, String: "unexpected '>'"}
	default:
		if lex.eatNewline(ch) {
			return Object{Kind: NL}
		}
		if strings.IndexByte(whitespace, ch) >= 0 {
			return lex.Next()
		}
		return Object{Kind: End, String: "unexpected input"}
	}
}

// -----------------------------------------------------------------------------

// FIXME: Lines /should not/ be longer than 255 characters,
// some wrapping is in order.
func (o *Object) Serialize() string {
	switch o.Kind {
	case NL:
		return "\n"
	case Nil:
		return "null"
	case Bool:
		if o.Number != 0 {
			return "true"
		}
		return "false"
	case Numeric:
		return strconv.FormatFloat(o.Number, 'f', -1, 64)
	case Keyword:
		return o.String
	case Name:
		escaped := []byte{'/'}
		for _, ch := range []byte(o.String) {
			escaped = append(escaped, ch)
			if ch == '#' || strings.IndexByte(delimiters+whitespace, ch) >= 0 {
				escaped = append(escaped, fmt.Sprintf("%02x", ch)...)
			}
		}
		return string(escaped)
	case String:
		escaped := []byte{'('}
		for _, ch := range []byte(o.String) {
			if ch == '\\' || ch == '(' || ch == ')' {
				escaped = append(escaped, '\\')
			}
			escaped = append(escaped, ch)
		}
		return string(append(escaped, ')'))
	case BArray:
		return "["
	case EArray:
		return "]"
	case BDict:
		return "<<"
	case EDict:
		return ">>"
	case Array:
		var v []string
		for _, i := range o.Array {
			v = append(v, i.Serialize())
		}
		return "[ " + strings.Join(v, " ") + " ]"
	case Dict:
		b := bytes.NewBuffer(nil)
		var keys []string
		for k := range o.Dict {
			keys = append(keys, k)
		}
		sort.Strings(keys)
		for _, k := range keys {
			v := o.Dict[k]
			// FIXME: The key is also supposed to be escaped by Serialize.
			fmt.Fprint(b, " /", k, " ", v.Serialize())
		}
		return "<<" + b.String() + " >>"
	case Indirect:
		return fmt.Sprintf("%d %d obj\n%s\nendobj", o.N, o.Generation,
			o.Array[0].Serialize())
	case Reference:
		return fmt.Sprintf("%d %d R", o.N, o.Generation)
	default:
		panic("unsupported token for serialization")
	}
}

// -----------------------------------------------------------------------------

type ref struct {
	offset     int64 // file offset or N of the next free entry
	generation uint  // object generation
	nonfree    bool  // whether this N is taken (for a good zero value)
}

// Updater is a utility class to help read and possibly incrementally update
// PDF files.
type Updater struct {
	// cross-reference table
	xref []ref

	// current cross-reference table size, correlated to len(xref)
	xrefSize uint

	// list of updated objects
	// TODO(p): A map to bool makes this simpler to work with.
	// The same with another map to struct{} somewhere in this code.
	updated map[uint]struct{}

	// PDF document data
	Document []byte

	// the new trailer dictionary to be written, initialized with the old one
	Trailer map[string]Object
}

func (u *Updater) parseIndirect(lex *Lexer, stack *[]Object) Object {
	lenStack := len(*stack)
	if lenStack < 2 {
		return Object{Kind: End, String: "missing object ID pair"}
	}

	n := (*stack)[lenStack-2]
	g := (*stack)[lenStack-1]
	*stack = (*stack)[:lenStack-2]

	if !g.IsUint() || !n.IsUint() {
		return Object{Kind: End, String: "invalid object ID pair"}
	}

	obj := Object{
		Kind: Indirect, N: uint(n.Number), Generation: uint(g.Number)}
	for {
		object := u.parse(lex, &obj.Array)
		if object.Kind == End {
			return Object{Kind: End, String: "object doesn't end"}
		}
		if object.Kind == Keyword && object.String == "endobj" {
			break
		}
		obj.Array = append(obj.Array, object)
	}
	return obj
}

func (u *Updater) parseR(stack *[]Object) Object {
	lenStack := len(*stack)
	if lenStack < 2 {
		return Object{Kind: End, String: "missing reference ID pair"}
	}

	n := (*stack)[lenStack-2]
	g := (*stack)[lenStack-1]
	*stack = (*stack)[:lenStack-2]

	if !g.IsUint() || !n.IsUint() {
		return Object{Kind: End, String: "invalid reference ID pair"}
	}
	return Object{
		Kind: Reference, N: uint(n.Number), Generation: uint(g.Number)}
}

/// parse reads an object at the lexer's position. Not a strict parser.
func (u *Updater) parse(lex *Lexer, stack *[]Object) Object {
	switch token := lex.Next(); token.Kind {
	case NL, Comment:
		// These are not important to parsing,
		// not even for this procedure's needs.
		return u.parse(lex, stack)
	case BArray:
		var array []Object
		for {
			object := u.parse(lex, &array)
			if object.Kind == End {
				return Object{Kind: End, String: "array doesn't end"}
			}
			if object.Kind == EArray {
				break
			}
			array = append(array, object)
		}
		return Object{Kind: Array, Array: array}
	case BDict:
		var array []Object
		for {
			object := u.parse(lex, &array)
			if object.Kind == End {
				return Object{Kind: End, String: "dictionary doesn't end"}
			}
			if object.Kind == EDict {
				break
			}
			array = append(array, object)
		}
		if len(array)%2 != 0 {
			return Object{Kind: End, String: "unbalanced dictionary"}
		}
		dict := make(map[string]Object)
		for i := 0; i < len(array); i += 2 {
			if array[i].Kind != Name {
				return Object{
					Kind: End, String: "invalid dictionary key type"}
			}
			dict[array[i].String] = array[i+1]
		}
		return Object{Kind: Dict, Dict: dict}
	case Keyword:
		// Appears in the document body, typically needs
		// to access the cross-reference table.
		//
		// TODO(p): Use the xref to read /Length etc. once we
		// actually need to read such objects; presumably
		// streams can use the Object.String member.
		switch token.String {
		case "stream":
			return Object{Kind: End, String: "streams are not supported yet"}
		case "obj":
			return u.parseIndirect(lex, stack)
		case "R":
			return u.parseR(stack)
		}
		fallthrough
	default:
		return token
	}
}

func (u *Updater) loadXref(lex *Lexer, loadedEntries map[uint]struct{}) error {
	var throwawayStack []Object
	if keyword := u.parse(lex,
		&throwawayStack); keyword.Kind != Keyword || keyword.String != "xref" {
		return errors.New("invalid xref table")
	}
	for {
		object := u.parse(lex, &throwawayStack)
		if object.Kind == End {
			return errors.New("unexpected EOF while looking for the trailer")
		}
		if object.Kind == Keyword && object.String == "trailer" {
			break
		}

		second := u.parse(lex, &throwawayStack)
		if !object.IsUint() || !second.IsUint() {
			return errors.New("invalid xref section header")
		}

		start, count := uint(object.Number), uint(second.Number)
		for i := uint(0); i < count; i++ {
			off := u.parse(lex, &throwawayStack)
			gen := u.parse(lex, &throwawayStack)
			key := u.parse(lex, &throwawayStack)
			if !off.IsInteger() || off.Number < 0 ||
				off.Number > float64(len(u.Document)) ||
				!gen.IsInteger() || gen.Number < 0 || gen.Number > 65535 ||
				key.Kind != Keyword {
				return errors.New("invalid xref entry")
			}

			free := true
			if key.String == "n" {
				free = false
			} else if key.String != "f" {
				return errors.New("invalid xref entry")
			}

			n := start + i
			if _, ok := loadedEntries[n]; ok {
				continue
			}
			if lenXref := uint(len(u.xref)); n >= lenXref {
				u.xref = append(u.xref, make([]ref, n-lenXref+1)...)
			}
			loadedEntries[n] = struct{}{}

			u.xref[n] = ref{
				offset:     int64(off.Number),
				generation: uint(gen.Number),
				nonfree:    !free,
			}
		}
	}
	return nil
}

// -----------------------------------------------------------------------------

var haystackRE = regexp.MustCompile(`(?s:.*)\sstartxref\s+(\d+)\s+%%EOF`)

// Initialize builds the cross-reference table and prepares
// a new trailer dictionary.
func (u *Updater) Initialize() error {
	u.updated = make(map[uint]struct{})

	// We only need to look for startxref roughly within
	// the last kibibyte of the document.
	haystack := u.Document
	if len(haystack) > 1024 {
		haystack = haystack[len(haystack)-1024:]
	}

	m := haystackRE.FindSubmatch(haystack)
	if m == nil {
		return errors.New("cannot find startxref")
	}

	xrefOffset, _ := strconv.ParseInt(string(m[1]), 10, 64)
	lastXrefOffset := xrefOffset
	loadedXrefs := map[int64]struct{}{}
	loadedEntries := map[uint]struct{}{}

	var throwawayStack []Object
	for {
		if _, ok := loadedXrefs[xrefOffset]; ok {
			return errors.New("circular xref offsets")
		}
		if xrefOffset >= int64(len(u.Document)) {
			return errors.New("invalid xref offset")
		}

		lex := Lexer{u.Document[xrefOffset:]}
		if err := u.loadXref(&lex, loadedEntries); err != nil {
			return err
		}

		trailer := u.parse(&lex, &throwawayStack)
		if trailer.Kind != Dict {
			return errors.New("invalid trailer dictionary")
		}
		if len(loadedXrefs) == 0 {
			u.Trailer = trailer.Dict
		}
		loadedXrefs[xrefOffset] = struct{}{}

		prevOffset, ok := trailer.Dict["Prev"]
		if !ok {
			break
		}
		// FIXME: We don't check for size_t over or underflow.
		if !prevOffset.IsInteger() {
			return errors.New("invalid Prev offset")
		}
		xrefOffset = int64(prevOffset.Number)
	}

	u.Trailer["Prev"] = Object{
		Kind: Numeric, Number: float64(lastXrefOffset)}

	lastSize, ok := u.Trailer["Size"]
	if !ok || !lastSize.IsInteger() || lastSize.Number <= 0 {
		return errors.New("invalid or missing cross-reference table Size")
	}
	u.xrefSize = uint(lastSize.Number)
	return nil
}

// Get retrieves an object by its number and generation--may return
// Nil or End with an error.
func (u *Updater) Get(n, generation uint) Object {
	if n >= u.xrefSize {
		return Object{Kind: Nil}
	}

	ref := u.xref[n]
	if !ref.nonfree || ref.generation != generation ||
		ref.offset >= int64(len(u.Document)) {
		return Object{Kind: Nil}
	}

	lex := Lexer{u.Document[ref.offset:]}
	var stack []Object
	for {
		object := u.parse(&lex, &stack)
		if object.Kind == End {
			return object
		}
		if object.Kind != Indirect {
			stack = append(stack, object)
		} else if object.N != n || object.Generation != generation {
			return Object{Kind: End, String: "object mismatch"}
		} else {
			return object.Array[0]
		}
	}
}

// Allocate allocates a new object number.
func (u *Updater) Allocate() uint {
	n := u.xrefSize
	u.xrefSize++

	if u.xrefSize == 0 {
		panic("overflow")
	} else if lenXref := uint(len(u.xref)); lenXref < u.xrefSize {
		u.xref = append(u.xref, make([]ref, u.xrefSize-lenXref)...)
	}

	// We don't make sure it gets a subsection in the update yet because we
	// make no attempts at fixing the linked list of free items either.
	return n
}

// BytesWriter is an interface over a subset of bytes.Buffer methods.
type BytesWriter interface {
	Bytes() []byte
	Len() int
	Write(p []byte) (n int, err error)
	WriteByte(c byte) error
	WriteRune(r rune) (n int, err error)
	WriteString(s string) (n int, err error)
}

// Update appends an updated object to the end of the document.
func (u *Updater) Update(n uint, fill func(buf BytesWriter)) {
	oldRef := u.xref[n]
	u.updated[n] = struct{}{}
	u.xref[n] = ref{
		offset:     int64(len(u.Document) + 1),
		generation: oldRef.generation,
		nonfree:    true,
	}

	buf := bytes.NewBuffer(u.Document)
	fmt.Fprintf(buf, "\n%d %d obj\n", n, oldRef.generation)

	// Separately so that the callback can use w.Len() to get current offset.
	fill(buf)

	buf.WriteString("\nendobj")
	u.Document = buf.Bytes()
}

// FlushUpdates writes an updated cross-reference table and trailer.
func (u *Updater) FlushUpdates() {
	updated := make([]uint, 0, len(u.updated))
	for n := range u.updated {
		updated = append(updated, n)
	}
	sort.Slice(updated, func(i, j int) bool {
		return updated[i] < updated[j]
	})

	groups := make(map[uint]uint)
	for i := 0; i < len(updated); {
		start, count := updated[i], uint(1)
		for i++; i != len(updated) && updated[i] == start+count; i++ {
			count++
		}
		groups[start] = count
	}

	// Taking literally "Each cross-reference section begins with a line
	// containing the keyword xref. Following this line are one or more
	// cross-reference subsections." from 3.4.3 in PDF Reference.
	if len(groups) == 0 {
		groups[0] = 0
	}

	buf := bytes.NewBuffer(u.Document)
	startXref := buf.Len() + 1
	buf.WriteString("\nxref\n")

	for start, count := range groups {
		fmt.Fprintf(buf, "%d %d\n", start, count)
		for i := uint(0); i < count; i++ {
			ref := u.xref[start+uint(i)]
			if ref.nonfree {
				fmt.Fprintf(buf, "%010d %05d n \n", ref.offset, ref.generation)
			} else {
				fmt.Fprintf(buf, "%010d %05d f \n", ref.offset, ref.generation)
			}
		}
	}

	u.Trailer["Size"] = Object{Kind: Numeric, Number: float64(u.xrefSize)}
	trailer := Object{Kind: Dict, Dict: u.Trailer}

	fmt.Fprintf(buf, "trailer\n%s\nstartxref\n%d\n%%%%EOF\n",
		trailer.Serialize(), startXref)
	u.Document = buf.Bytes()
}

// -----------------------------------------------------------------------------

// NewDate makes a PDF object representing the given point in time.
func NewDate(ts time.Time) Object {
	buf := ts.AppendFormat(nil, "D:20060102150405")
	// "Z07'00'" doesn't work, we need to do some of it manually.
	if _, offset := ts.Zone(); offset != 0 {
		o := ts.AppendFormat(nil, "-0700")
		buf = append(buf, o[0], o[1], o[2], '\'', o[3], o[4], '\'')
	} else {
		buf = append(buf, 'Z')
	}
	return Object{Kind: String, String: string(buf)}
}

// GetFirstPage retrieves the first page of the document or a Nil object.
func GetFirstPage(pdf *Updater, nodeN, nodeGeneration uint) Object {
	obj := pdf.Get(nodeN, nodeGeneration)
	if obj.Kind != Dict {
		return Object{Kind: Nil}
	}

	// Out of convenience; these aren't filled normally.
	obj.N = nodeN
	obj.Generation = nodeGeneration

	if typ, ok := obj.Dict["Type"]; !ok || typ.Kind != Name {
		return Object{Kind: Nil}
	} else if typ.String == "Page" {
		return obj
	} else if typ.String != "Pages" {
		return Object{Kind: Nil}
	}

	// XXX: Technically speaking, this may be an indirect reference.
	// The correct way to solve this seems to be having Updater include
	// a wrapper around "obj.Dict". Though does it still apply in Golang?
	kids, ok := obj.Dict["Kids"]
	if !ok || kids.Kind != Array || len(kids.Array) == 0 ||
		kids.Array[0].Kind != Reference {
		return Object{Kind: Nil}
	}

	// XXX: Nothing prevents us from recursing in an evil circular graph.
	return GetFirstPage(pdf, kids.Array[0].N, kids.Array[0].Generation)
}

// -----------------------------------------------------------------------------

// PKCS12Parse parses and verifies PKCS#12 data.
func PKCS12Parse(p12 []byte, password string) (
	crypto.PrivateKey, []*x509.Certificate, error) {
	// The pkcs12.Decode function doesn't support included intermediate
	// certificates, we need to do some processing manually.
	blocks, err := pkcs12.ToPEM(p12, password)
	if err != nil {
		return nil, nil, err
	}

	// b.Type is literally CERTIFICATE or PRIVATE KEY, the Headers only contain
	// a localKeyId field. It seems like the pkey and the cert share the same
	// localKeyId value. Though the leaf certificate should also be the first
	// one in the PKCS#12 file, so I probably don't need that value.
	var allX509Blocks [][]byte
	var allCertBlocks [][]byte
	for _, b := range blocks {
		// CERTIFICATE, PRIVATE KEY constants are defined locally in the pkcs12
		// package. crypto/tls/tls.go seems to only use literals for these and
		// also accepts words in front such as RSA PRIVATE KEY.
		switch b.Type {
		case "PRIVATE KEY":
			allX509Blocks = append(allX509Blocks, b.Bytes)
		case "CERTIFICATE":
			allCertBlocks = append(allCertBlocks, b.Bytes)
		}
	}
	switch {
	case len(allX509Blocks) == 0:
		return nil, nil, errors.New("missing private key")
	case len(allX509Blocks) > 1:
		return nil, nil, errors.New("more than one private key")
	case len(allCertBlocks) == 0:
		return nil, nil, errors.New("missing certificate")
	}

	// The PKCS#12 file may only contain PKCS#8-wrapped private keys but the
	// pkcs12 package unwraps them to simple PKCS#1/EC while converting to PEM.
	var key crypto.PrivateKey
	if key, err = x509.ParsePKCS1PrivateKey(allX509Blocks[0]); err != nil {
		if key, err = x509.ParseECPrivateKey(allX509Blocks[0]); err == nil {
			return nil, nil, errors.New("failed to parse private key")
		}
	}

	x509Certs, err := x509.ParseCertificates(allCertBlocks[0])
	if err != nil {
		return nil, nil, err
	}
	if len(x509Certs) != 1 {
		return nil, nil,
			errors.New("expected exactly one certificate in the first bag")
	}

	for _, cert := range allCertBlocks[1:] {
		toAdd, err := x509.ParseCertificates(cert)
		if err != nil {
			return nil, nil, err
		}
		x509Certs = append(x509Certs, toAdd...)
	}

	// Copied from crypto/tls/tls.go.
	switch pub := x509Certs[0].PublicKey.(type) {
	case *rsa.PublicKey:
		priv, ok := key.(*rsa.PrivateKey)
		if !ok {
			return nil, nil,
				errors.New("private key type does not match public key type")
		}
		if pub.N.Cmp(priv.N) != 0 {
			return nil, nil,
				errors.New("private key does not match public key")
		}
	case *ecdsa.PublicKey:
		priv, ok := key.(*ecdsa.PrivateKey)
		if !ok {
			return nil, nil,
				errors.New("private key type does not match public key type")
		}
		if pub.X.Cmp(priv.X) != 0 || pub.Y.Cmp(priv.Y) != 0 {
			return nil, nil,
				errors.New("private key does not match public key")
		}
	default:
		return nil, nil, errors.New("unknown public key algorithm")
	}
	return key, x509Certs, nil
}

// FillInSignature signs PDF contents and writes the signature into the given
// window that has been reserved for this specific purpose.
func FillInSignature(document []byte, signOff, signLen int,
	key crypto.PublicKey, certs []*x509.Certificate) error {
	if signOff < 0 || signOff > len(document) ||
		signLen < 2 || signOff+signLen > len(document) {
		return errors.New("invalid signing window")
	}

	pkcsError := func(message interface{}) error {
		return fmt.Errorf("key/cert: %s", message)
	}

	// Prevent useless signatures--makes pdfsig from poppler happy at least
	// (and NSS by extension).
	x509Cert := certs[0]
	if x509Cert.KeyUsage&(x509.KeyUsageDigitalSignature|
		x509.KeyUsageContentCommitment /* renamed non-repudiation */) == 0 {
		return pkcsError("the certificate's key usage must include " +
			"digital signatures or non-repudiation")
	}

	extOK := false
	for _, u := range x509Cert.ExtKeyUsage {
		if u == x509.ExtKeyUsageAny || u == x509.ExtKeyUsageEmailProtection {
			extOK = true
		}
	}
	if len(x509Cert.ExtKeyUsage) > 0 && !extOK {
		return pkcsError("the certificate's extended key usage " +
			"must include S/MIME")
	}

	// XXX: We'd like to stream to the hash manually instead of copying data.
	data := make([]byte, len(document)-signLen)
	copy(data, document[:signOff])
	copy(data[signOff:], document[signOff+signLen:])

	signedData, err := pkcs7.NewSignedData(data)
	if err != nil {
		return err
	}
	// The default digest is SHA1, which is mildly insecure now.
	signedData.SetDigestAlgorithm(pkcs7.OIDDigestAlgorithmSHA256)
	if err := signedData.AddSignerChain(
		x509Cert, key, certs[1:], pkcs7.SignerInfoConfig{}); err != nil {
		return err
	}

	signedData.Detach()
	sig, err := signedData.Finish()
	if err != nil {
		return err
	}

	/*
		Debugging: ioutil.WriteFile("pdf_signature.der", sig, 0666)
		openssl cms -inform PEM -in pdf_signature.pem -noout -cmsout -print
		Context: https://stackoverflow.com/a/29253469
	*/

	if len(sig)*2 > signLen-2 /* hexstring quotes */ {
		// The obvious solution is to increase the allocation... or spend
		// a week reading specifications while losing all faith in humanity
		// as a species, and skip the pkcs7 package entirely.
		return fmt.Errorf("not enough space reserved for the signature "+
			"(%d nibbles vs %d nibbles)", signLen-2, len(sig)*2)
	}

	hex.Encode(document[signOff+1:], sig)
	return nil
}

// https://www.adobe.com/devnet-docs/acrobatetk/tools/DigSig/Acrobat_DigitalSignatures_in_PDF.pdf
// https://www.adobe.com/content/dam/acom/en/devnet/acrobat/pdfs/pdf_reference_1-7.pdf
// https://www.adobe.com/content/dam/acom/en/devnet/acrobat/pdfs/PPKAppearances.pdf

// Sign signs the given document, growing and returning the passed-in slice.
// There must be at least one certificate, matching the private key.
// The certificates must form a chain.
//
// The presumption here is that the document is valid and that it doesn't
// employ cross-reference streams from PDF 1.5, or at least constitutes
// a hybrid-reference file. The results with PDF 2.0 (2017) are currently
// unknown as the standard costs money.
//
// Carelessly assumes that the version of the original document is at most
// PDF 1.6.
func Sign(document []byte, key crypto.PrivateKey, certs []*x509.Certificate) (
	[]byte, error) {
	pdf := &Updater{Document: document}
	if err := pdf.Initialize(); err != nil {
		return nil, err
	}

	rootRef, ok := pdf.Trailer["Root"]
	if !ok || rootRef.Kind != Reference {
		return nil, errors.New("trailer does not contain a reference to Root")
	}
	root := pdf.Get(rootRef.N, rootRef.Generation)
	if root.Kind != Dict {
		return nil, errors.New("invalid Root dictionary reference")
	}

	// 8.7 Digital Signatures - /signature dictionary/
	sigdictN := pdf.Allocate()
	var byterangeOff, byterangeLen, signOff, signLen int
	pdf.Update(sigdictN, func(buf BytesWriter) {
		// The timestamp is important for Adobe Acrobat Reader DC.
		// The ideal would be to use RFC 3161.
		now := NewDate(time.Now())
		buf.WriteString("<< /Type/Sig /Filter/Adobe.PPKLite" +
			" /SubFilter/adbe.pkcs7.detached\n" +
			"   /M" + now.Serialize() + " /ByteRange ")

		byterangeOff = buf.Len()
		byterangeLen = 32 // fine for a gigabyte
		buf.Write(bytes.Repeat([]byte{' '}, byterangeLen))
		buf.WriteString("\n   /Contents <")

		signOff = buf.Len()
		signLen = 8192 // cert, digest, encripted digest, ...
		buf.Write(bytes.Repeat([]byte{'0'}, signLen))
		buf.WriteString("> >>")

		// We actually need to exclude the hexstring quotes from signing.
		signOff -= 1
		signLen += 2
	})

	sigfield := Object{Kind: Dict, Dict: map[string]Object{
		// 8.6.3 Field Types - Signature Fields
		"FT": {Kind: Name, String: "Sig"},
		"V":  {Kind: Reference, N: sigdictN, Generation: 0},
		// 8.4.5 Annotations Types - Widget Annotations
		// We can merge the Signature Annotation and omit Kids here.
		"Subtype": {Kind: Name, String: "Widget"},
		"F":       {Kind: Numeric, Number: 2 /* Hidden */},
		"T":       {Kind: String, String: "Signature1"},
		"Rect": {Kind: Array, Array: []Object{
			{Kind: Numeric, Number: 0},
			{Kind: Numeric, Number: 0},
			{Kind: Numeric, Number: 0},
			{Kind: Numeric, Number: 0},
		}},
	}}

	sigfieldN := pdf.Allocate()
	pdf.Update(sigfieldN, func(buf BytesWriter) {
		buf.WriteString(sigfield.Serialize())
	})

	pagesRef, ok := root.Dict["Pages"]
	if !ok || pagesRef.Kind != Reference {
		return nil, errors.New("invalid Pages reference")
	}
	page := GetFirstPage(pdf, pagesRef.N, pagesRef.Generation)
	if page.Kind != Dict {
		return nil, errors.New("invalid or unsupported page tree")
	}

	// XXX: Assuming this won't be an indirectly referenced array.
	annots := page.Dict["Annots"]
	if annots.Kind != Array {
		annots = Object{Kind: Array}
	}
	annots.Array = append(annots.Array, Object{
		Kind: Reference, N: sigfieldN, Generation: 0})

	page.Dict["Annots"] = annots
	pdf.Update(page.N, func(buf BytesWriter) {
		buf.WriteString(page.Serialize())
	})

	// 8.6.1 Interactive Form Dictionary
	// XXX: Assuming there are no forms already, overwriting everything.
	root.Dict["AcroForm"] = Object{Kind: Dict, Dict: map[string]Object{
		"Fields": {Kind: Array, Array: []Object{
			{Kind: Reference, N: sigfieldN, Generation: 0},
		}},
		"SigFlags": {Kind: Numeric,
			Number: 3 /* SignaturesExist | AppendOnly */},
	}}

	// Upgrade the document version for SHA-256 etc.
	// XXX: Assuming that it's not newer than 1.6 already--while Cairo can't
	// currently use a newer version that 1.5, it's not a bad idea to use
	// cairo_pdf_surface_restrict_to_version().
	root.Dict["Version"] = Object{Kind: Name, String: "1.6"}
	pdf.Update(rootRef.N, func(buf BytesWriter) {
		buf.WriteString(root.Serialize())
	})
	pdf.FlushUpdates()

	// Now that we know the length of everything, store byte ranges of
	// what we're about to sign, which must be everything but the resulting
	// signature itself.
	tailOff := signOff + signLen
	tailLen := len(pdf.Document) - tailOff

	ranges := fmt.Sprintf("[0 %d %d %d]", signOff, tailOff, tailLen)
	if len(ranges) > byterangeLen {
		return nil, errors.New("not enough space reserved for /ByteRange")
	}
	copy(pdf.Document[byterangeOff:], []byte(ranges))
	if err := FillInSignature(pdf.Document, signOff, signLen,
		key, certs); err != nil {
		return nil, err
	}
	return pdf.Document, nil
}
