#!/usr/bin/perl -w

use strict;

# Given a subdirectory of images, inserts each image into the
# object database with the appropriate GID, both as a
# reduced-resolution PPM and as the full-resolution original.

# Attributes:
# - Tags originals with a GID and reduced-resolution ones with
#   a different GID.  Both GIDs are fresh (taken by scanning the
#   gid.map file in the current directory) [limiting? TODO]
# - Content-Type: set to the appropriate image type
# - Original-Ref: attribute exists in reduced resolution image
#   only, and gives the OID of the original image.

die "Usage $0 directory [machine1 ... machinen]" unless scalar(@ARGV) >= 1;

# print "$0 ", join(' ', @ARGV), "\n";

# List of known image types (extensions)
# The key is extension, value is the Content-Type MIME string
my %valid = (
    jpeg => 'image/jpeg',
    jpg => 'image/jpeg',
    gif => 'image/gif',
    ppm => 'image/x-portable-pixmap',
    pgm => 'image/x-portable-graymap',
    png => 'image/png'
  );

# This will probably change TODO
my $insert_command = "apitest";

# Temporary file used for conversion purposes
my $tempext = 'ppm';
my $tempfile = "/tmp/diamond-ppm-$$.$tempext";
die "The tempfile ($tempfile) already exists." if (-e $tempfile);

# Hashtable of gids that have been used (defined in gid_map
# or during this run of the program).
my %used_gid = ();

# my $root = $ARGV[0];
my $root = shift(@ARGV);
$root =~ s/\/$//;	# Chop trailing / away
my $machines = @ARGV ? join(' ', @ARGV) : "localhost";


my $count = 0;	# Number of files inserted so far

&process_gidmapfile();

# Two unique, unused gids.
# gid1 is for the originals
# gid2 is for the reduced-resolution ones
my $gid1 = &generate_gid;
my $gid2 = &generate_gid;
open (GIDMAP, ">>gid_map") or die "Unable to append ./gid_map";
print GIDMAP "#\n# $root\n$gid1\t$machines\n$gid2\t$machines\n";
close GIDMAP;

&process_directory($root);



########################################
# Subroutines

# Processes gid_map file to fill the %used_gid hash
#
sub process_gidmapfile {
# TODO

  open (GIDMAP, "gid_map") or die "Unable to read ./gid_map";
  while (<GIDMAP>) {
    chomp;
    next if /^\s*$/ or /^#/;
    my ($g) = /^([:\dabcdefABCDEF]+)/;	# Colon separated hex string
    $g =~ s/://g;
    $g = "\L$g";
    $used_gid{$g}++;
  }
  close GIDMAP;
}

# Returns an unused gid.
# Currently generates gids randomly, but it's probably better to
# use sequential generation of gids.
#
sub generate_gid {
  my $h;
  do {
    $h = &get_random_uint64;
  } while $used_gid{$h};
  $used_gid{$h}++;
  print "Generated new GID: $h\n";
  return $h;
}


# Returns a string in hex that is a uint64
#
sub get_random_uint64 {
  my $x = "";
  for (1 .. 8) {
    $x .= (0 .. 9, 'a' .. 'f') [ int(rand(16)) ];
    $x .= (0 .. 9, 'a' .. 'f') [ int(rand(16)) ];
    $x .= ":";
  }
  chop($x);
  return $x;
}

# Recursively processes all of the files starting at this subdir
# The first argument is the root (not used in displayname)
# Second argument is path from root.
# Together, they give the full path name of directory.
#
sub process_directory {
  my ($root, $path) = @_;
  my $indir = $path ? "$root/$path" : $root;

  print "Processing directory <$indir>\n";
  die "$indir is not a directory" unless (-d $indir);

  opendir(DIR, $indir) or die "Unable to opendir $indir: $!";
  foreach (readdir(DIR)) {
    next if ($_ eq '.') or ($_ eq '..');
    my $f = "$indir/$_";
#    print "$f\n";
    my $foo = $path ? "$path/$_" : $_;
    if (-d $f) { &process_directory($root, $foo); }
    else { &process_image($foo, $f); }
  }
  closedir(DIR);
}


sub process_image {
  my $path = shift;	# Image name from $root
  my $img = shift;	# Complete name of image
  my ($ext) = ($img =~ /\.(\w+)$/);
  $ext = "\L$ext";	# Canonicalize to lower case
  unless ($valid{$ext}) {
    print "Skipping $img (unknown type)\n";
    return;
  }

  my $dispname = $path;
  $dispname =~ s|/|:|g;

  my ($exec, $retval);

  $exec = "$insert_command $img $gid1 " .
	     "Display-Name $dispname " .
	     "Content-Type " . $valid{$ext};
  print "$exec\n";
  $retval = system($exec);
  die "$insert_command failed with return code of $retval: $!" if $retval;


  $retval = system("convert $img -resize 400x300 $tempfile");
  die "convert gave a return code of $retval: $!" if $retval;
  $exec = "$insert_command $tempfile $gid2 " .
	     "Content-Type " . $valid{$ext} . " " .
	     "Parent-OID 00:00:00:00:00:00:00:00";	# TODO
  print "$exec\n";
  $retval = system($exec);
  die "$insert_command failed with return code of $retval: $!" if $retval;

  print "---\n";
  unlink $tempfile or die "Unable to delete $tempfile";
}

#  print $count++, ": Insert $img with gid=$gid1 (TODO)\n";
#  print "Attr: Display-Name => $dispname\n";
#  print "Attr: Content-Type => ", $valid{$ext}, "\n";

#  print "Insert $tempfile with gid=$gid2 (TODO)\n";
#  print "Attr: Content-Type => ", $valid{$tempext}, "\n";
#  print "Attr: Parent-oid => (whatever returned above) (TODO)\n";