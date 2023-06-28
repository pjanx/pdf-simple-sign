#!/bin/sh -e
# Test basic functionality of both versions
# Usage: ./test.sh builddir/pdf-simple-sign cmd/pdf-simple-sign/pdf-simple-sign

log() { echo "`tput sitm`-- $1`tput sgr0`"; }
die() { echo "`tput bold`-- $1`tput sgr0`"; exit 1; }

# Get rid of old test files
rm -rf tmp
mkdir tmp

# Create documents in various tools
log "Creating source documents"
inkscape --pipe --export-filename=tmp/cairo.pdf --export-pdf-version=1.4 \
<<'EOF' 2>/dev/null || :
<svg xmlns="http://www.w3.org/2000/svg"><text x="5" y="10">Hello</text></svg>
EOF

date | tee tmp/lowriter.txt | groff -T pdf > tmp/groff.pdf || :
lowriter --convert-to pdf tmp/lowriter.txt --outdir tmp >/dev/null || :
convert rose: tmp/imagemagick.pdf || :

# Create a root CA certificate pair
log "Creating certificates"
openssl req -newkey rsa:2048 -subj "/CN=Test CA" -nodes \
	-keyout tmp/ca.key.pem -x509 -out tmp/ca.cert.pem 2>/dev/null

# Create a private NSS database and insert our test CA there
rm -rf tmp/nssdir
mkdir tmp/nssdir
certutil -N --empty-password -d sql:tmp/nssdir
certutil -d sql:tmp/nssdir -A -n root -t ,C, -a -i tmp/ca.cert.pem

# Create a leaf certificate pair
cat > tmp/cert.cfg <<'EOF'
[smime]
basicConstraints = CA:FALSE
keyUsage = digitalSignature
extendedKeyUsage = emailProtection
nsCertType = email
EOF

openssl req -newkey rsa:2048 -subj "/CN=Test Leaf" -nodes \
	-keyout tmp/key.pem -out tmp/cert.csr 2>/dev/null
openssl x509 -req -in tmp/cert.csr -out tmp/cert.pem \
	-CA tmp/ca.cert.pem -CAkey tmp/ca.key.pem -set_serial 1 \
	-extensions smime -extfile tmp/cert.cfg 2>/dev/null
openssl verify -CAfile tmp/ca.cert.pem tmp/cert.pem >/dev/null

# The second line accomodates the Go signer,
# which doesn't support SHA-256 within pkcs12 handling
openssl pkcs12 -inkey tmp/key.pem -in tmp/cert.pem \
	-certpbe PBE-SHA1-3DES -keypbe PBE-SHA1-3DES -macalg sha1 \
	-export -passout pass: -out tmp/key-pair.p12

for tool in "$@"; do
	rm -f tmp/*.signed.pdf
	for source in tmp/*.pdf; do
		log "Testing $tool with $source"
		result=${source%.pdf}.signed.pdf
		$tool "$source" "$result" tmp/key-pair.p12 ""
		pdfsig -nssdir sql:tmp/nssdir "$result" | grep Validation

		# Only some of our generators use PDF versions higher than 1.5
		log "Testing $tool for version detection"
		grep -q "/Version /1.6" "$result" || grep -q "^%PDF-1.6" "$result" \
			|| die "Version detection seems to misbehave (no upgrade)"
	done

	log "Testing $tool for expected failures"
	$tool "$result" "$source.fail.pdf" tmp/key-pair.p12 "" \
		&& die "Double signing shouldn't succeed"
	$tool -r 1 "$source" "$source.fail.pdf" tmp/key-pair.p12 "" \
		&& die "Too low reservations shouldn't succeed"

	sed '1s/%PDF-1../%PDF-1.7/' "$source" > "$source.alt"
	$tool "$source.alt" "$result.alt" tmp/key-pair.p12 ""
	grep -q "/Version /1.6" "$result.alt" \
		&& die "Version detection seems to misbehave (downgraded)"
done

log "OK"
