#!/bin/sh

# Copy file if it exists
# Usage: Set SCRIPT_INPUT_FILE_0 and SCRIPT_OUTPUT_FILE_0 in Xcode
# Or pass input and output paths as arguments

# Use POSIX parameter expansion for default values
INPUT_FILE=${SCRIPT_INPUT_FILE_0:-${1}}
OUTPUT_FILE=${SCRIPT_OUTPUT_FILE_0:-${2}}

if [ -z "$INPUT_FILE" ] || [ -z "$OUTPUT_FILE" ]; then
    echo "Error: Input file and output file must be specified"
    echo "Usage: $0 <input_file> <output_file>"
    echo "Or set SCRIPT_INPUT_FILE_0 and SCRIPT_OUTPUT_FILE_0 environment variables"
    exit 1
fi

if [ ! -f "$INPUT_FILE" ]; then
    echo "Skipping copy: Input file does not exist: $INPUT_FILE"
    exit 0
fi

OUTPUT_DIR=$(dirname "$OUTPUT_FILE")
if [ ! -d "$OUTPUT_DIR" ]; then
    echo "Creating output directory: $OUTPUT_DIR"
    mkdir -p "$OUTPUT_DIR"
fi

echo "Copying: $INPUT_FILE -> $OUTPUT_FILE"
cp "$INPUT_FILE" "$OUTPUT_FILE"

if [ $? -eq 0 ]; then
    echo "Copy successful"
else
    echo "Error: Copy failed"
    exit 1
fi
