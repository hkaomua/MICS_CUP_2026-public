#!/bin/sh
set -eu
slide_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build_dir="$slide_dir/../build/slides/uptex"
output_dir="$slide_dir/output/pdf"
mkdir -p "$build_dir" "$output_dir"
cd "$slide_dir/template/beamer"
latexmk -outdir="$build_dir" "$slide_dir/nescity_optimization.tex" \
  > "$build_dir/latexmk.log" 2>&1 || {
    tail -n 60 "$build_dir/latexmk.log"
    exit 1
  }
cp "$build_dir/nescity_optimization.pdf" "$output_dir/nescity_optimization.pdf"
echo "$output_dir/nescity_optimization.pdf"
