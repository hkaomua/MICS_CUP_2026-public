#!/bin/sh
set -eu
slide_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build_dir="$slide_dir/../.build/slides/uptex"
output_dir="$slide_dir/output/pdf"
mkdir -p "$build_dir" "$output_dir"
python3 "$slide_dir/generate_metrics.py" --output "$build_dir/metrics.tex"
cd "$slide_dir/template/beamer"
# Use the upstream engine/font configuration without modifying the theme.
export TEXINPUTS="$build_dir//:${TEXINPUTS:-}"
latexmk -outdir="$build_dir" "$slide_dir/nescity_fpga_optimization.tex" \
  > "$build_dir/latexmk.log" 2>&1 || {
    tail -n 60 "$build_dir/latexmk.log" >&2
    exit 1
  }
cp "$build_dir/nescity_fpga_optimization.pdf" "$output_dir/nescity_fpga_optimization.pdf"
echo "$output_dir/nescity_fpga_optimization.pdf"
