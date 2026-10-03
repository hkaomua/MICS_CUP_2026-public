use Config;
$pdf_mode = 3;
$latex = 'uplatex -interaction=nonstopmode -halt-on-error -file-line-error %O %S';
$dvipdf = 'dvipdfmx %O -o %D %S';
$out_dir = 'build';
$ENV{'OPENTYPEFONTS'} = '../fonts//' . $Config{path_sep} . ($ENV{'OPENTYPEFONTS'} // '');
$ENV{'TFMFONTS'} = '../fonts//' . $Config{path_sep} . ($ENV{'TFMFONTS'} // '');
$ENV{'CMAPFONTS'} = '../fonts//' . $Config{path_sep} . ($ENV{'CMAPFONTS'} // '');
