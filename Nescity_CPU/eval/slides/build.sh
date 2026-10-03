#!/bin/sh
set -eu
slide_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build_dir="$slide_dir/../build/slides"
output_dir="$slide_dir/output/pdf"
mkdir -p "$build_dir" "$output_dir"
for pass in 1 2; do
  xelatex -no-shell-escape -interaction=nonstopmode -halt-on-error \
    -output-directory="$build_dir" "$slide_dir/nescity_optimization.tex" \
    > "$build_dir/latex-pass-$pass.log"
done
cp "$build_dir/nescity_optimization.pdf" "$output_dir/nescity_optimization.pdf"
echo "$output_dir/nescity_optimization.pdf"
