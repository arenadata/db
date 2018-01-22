#! /usr/bin/perl -w

#################################################################
# create_help.pl -- converts SGML docs to internal psql help
#
# Copyright (c) 2000-2010, PostgreSQL Global Development Group
#
# src/bin/psql/create_help.pl
#################################################################

#
# This script automatically generates the help on SQL in psql from
# the SGML docs. So far the format of the docs was consistent
# enough that this worked, but this here is by no means an SGML
# parser.
#
# Call: perl create_help.pl docdir sql_help
# The name of the header file doesn't matter to this script, but it
# sure does matter to the rest of the source.
#

use strict;

my $docdir = $ARGV[0] or die "$0: missing required argument: docdir\n";
my $hfile = $ARGV[1] . '.h' or die "$0: missing required argument: output file\n";
my $cfile = $ARGV[1] . '.c';

my $hfilebasename;
if ($hfile =~ m!.*/([^/]+)$!) {
    $hfilebasename = $1;
}
else {
    $hfilebasename = $hfile;
}

my $define = $hfilebasename;
$define =~ tr/a-z/A-Z/;
$define =~ s/\W/_/g;

opendir(DIR, $docdir)
    or die "$0: could not open documentation source dir '$docdir': $!\n";
open(HFILE, ">$hfile")
    or die "$0: could not open output file '$hfile': $!\n";
open(CFILE, ">$cfile")
    or die "$0: could not open output file '$cfile': $!\n";

print HFILE
"/*
 * *** Do not change this file by hand. It is automatically
 * *** generated from the DocBook documentation.
 *
 * generated by
 *     $^X $0 @ARGV
 *
 */

#ifndef $define
#define $define

#define N_(x) (x)				/* gettext noop */

#include \"postgres_fe.h\"
#include \"pqexpbuffer.h\"

struct _helpStruct
{
	const char	   *cmd;		/* the command name */
	const char	   *help;		/* the help associated with it */
	void (*syntaxfunc)(PQExpBuffer);	/* function that prints the syntax associated with it */
	int				nl_count;	/* number of newlines in syntax (for pager) */
};

";

print CFILE
"/*
 * *** Do not change this file by hand. It is automatically
 * *** generated from the DocBook documentation.
 *
 * generated by
 *     $^X $0 @ARGV
 *
 */

#include \"$hfile\"

";

my $maxlen = 0;

my %entries;

foreach my $file (sort readdir DIR) {
    my (@cmdnames, $cmddesc, $cmdsynopsis);
    $file =~ /\.sgml$/ or next;

    open(FILE, "$docdir/$file") or next;
    my $filecontent = join('', <FILE>);
    close FILE;

    # Ignore files that are not for SQL language statements
    $filecontent =~ m!<refmiscinfo>\s*SQL - Language Statements\s*</refmiscinfo>!i
	or next;

    # Collect multiple refnames
    LOOP: { $filecontent =~ m!\G.*?<refname>\s*([a-z ]+?)\s*</refname>!cgis and push @cmdnames, $1 and redo LOOP; }
    $filecontent =~ m!<refpurpose>\s*(.+?)\s*</refpurpose>!is and $cmddesc = $1;
    $filecontent =~ m!<synopsis>\s*(.+?)\s*</synopsis>!is and $cmdsynopsis = $1;

    if (@cmdnames && $cmddesc && $cmdsynopsis) {
        s/\"/\\"/g foreach @cmdnames;

	$cmddesc =~ s/<[^>]+>//g;
	$cmddesc =~ s/\s+/ /g;
        $cmddesc =~ s/\"/\\"/g;

        my @params = ();

        my $nl_count = () = $cmdsynopsis =~ /\n/g;

        $cmdsynopsis =~ m!</>! and die "$0:$file: null end tag not supported in synopsis\n";
        $cmdsynopsis =~ s/%/%%/g;

        while ($cmdsynopsis =~ m!<(\w+)[^>]*>(.+?)</\1[^>]*>!) {
            my $match = $2;
	    $match =~ s/<[^>]+>//g;
	    $match =~ s/%%/%/g;
            push @params, $match;
            $cmdsynopsis =~ s!<(\w+)[^>]*>.+?</\1[^>]*>!%s!;
        }
	$cmdsynopsis =~ s/\r?\n/\\n/g;
        $cmdsynopsis =~ s/\"/\\"/g;

        foreach my $cmdname (@cmdnames) {
	    $entries{$cmdname} = { cmddesc => $cmddesc, cmdsynopsis => $cmdsynopsis, params => \@params, nl_count => $nl_count };
	    $maxlen = ($maxlen >= length $cmdname) ? $maxlen : length $cmdname;
	}
    }
    else {
	die "$0: parsing file '$file' failed (N='@cmdnames' D='$cmddesc')\n";
    }
}

foreach (sort keys %entries) {
    my $prefix = "\t"x5 . '  ';
    my $id = $_;
    $id =~ s/ /_/g;
    my $synopsis = "\"$entries{$_}{cmdsynopsis}\"";
    $synopsis =~ s/\\n/\\n"\n$prefix"/g;
    my @args = ("buf",
                $synopsis,
                map("_(\"$_\")", @{$entries{$_}{params}}));
    print HFILE "extern void sql_help_$id(PQExpBuffer buf);\n";
    print CFILE "void
sql_help_$id(PQExpBuffer buf)
{
\tappendPQExpBuffer(".join(",\n$prefix", @args).");
}

";
}

print HFILE "

static const struct _helpStruct QL_HELP[] = {
";
foreach (sort keys %entries) {
    my $id = $_;
    $id =~ s/ /_/g;
    print HFILE "    { \"$_\",
      N_(\"$entries{$_}{cmddesc}\"),
      sql_help_$id,
      $entries{$_}{nl_count} },

";
}

print HFILE "
    { NULL, NULL, NULL }    /* End of list marker */
};


#define QL_HELP_COUNT	".scalar(keys %entries)."		/* number of help items */
#define QL_MAX_CMD_LEN	$maxlen		/* largest strlen(cmd) */


#endif /* $define */
";

close CFILE;
close HFILE;
closedir DIR;
