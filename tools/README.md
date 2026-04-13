# dyld Tools

This directory contains utility scripts and tools for dyld development.

## archive-crcer.py

A Python script for stamping CRC32c checksums into AppleArchive files. Used to generate test data for atlas checksum validation tests.

### Usage

The tool scans an AppleArchive file for `com.apple.dyld.crc32c` extended attribute markers and stamps the correct record length and CRC32c values.

```bash
./archive-crcer.py <archive.aa>
```

No external dependencies required - uses a pure Python CRC32c implementation.

### Creating Test Archives

1. Create test files with placeholder xattr values:
```bash
mkdir -p /tmp/test/data
echo -n "foo" > /tmp/test/data/Baz
echo -n "bar" > /tmp/test/Bar
xattr -wx com.apple.dyld.crc32c "00 00 00 00 00 00 00 00" /tmp/test/Bar
xattr -wx com.apple.dyld.crc32c "00 00 00 00 00 00 00 00" /tmp/test/data/Baz
```

2. Create the archive with raw encoding:
```bash
aa archive -d /tmp/test -o /tmp/test.aa -a raw \
  -exclude-field all -include-field typ,pat,dat,lnk,xat
```

3. Stamp the CRCs:
```bash
./archive-crcer.py /tmp/test.aa
```

4. Verify with hexdump:
```bash
hexdump -C /tmp/test.aa
```
