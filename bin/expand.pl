#!/usr/bin/perl 

use strict;


my $sdk      = $ENV{"SDKROOT"};
my $availCmd = $sdk . "/usr/local/libexec/availability.pl";

sub expandVersions
{
	my $macroPrefix = shift;
	my $availArg = shift;

	my $cmd = $availCmd . " " . $availArg;
	my $versionList   = `$cmd`;
	my $tmp = $versionList;
	while ($tmp =~ m/^\s*([\S]+)(.*)$/) {
		my $vers = $1;
		$tmp = $2;

		my $major = 0;
		my $minor = 0;
		my $revision = 0;
		my $uvers;

		if ($vers =~ m/^(\d+)$/) {
			$major = $1;
			$uvers = sprintf("%d_0", $major);
		} elsif ($vers =~ m/^(\d+).(\d+)$/) {
			$major = $1;
			$minor = $2;
			$uvers = sprintf("%d_%d", $major, $minor);
		} elsif ($vers =~ m/^(\d+).(\d+).(\d+)$/) {
			$major = $1;
			$minor = $2;
			$revision = $3;
			if ($revision == 0) {
				$uvers = sprintf("%d_%d", $major, $minor);
			}
			else {
				$uvers = sprintf("%d_%d_%d", $major, $minor, $revision);
			}
		}
		printf "#define %s%-18s 0x00%02X%02X%02X\n", $macroPrefix, $uvers, $major, $minor, $revision;
	}
}




while(<STDIN>)
{
	if(m/^\/\/\@MAC_VERSION_DEFS\@$/) {
		expandVersions("DYLD_MACOSX_VERSION_", "--macosx");
	}
	elsif(m/^\/\/\@IOS_VERSION_DEFS\@$/) {
		expandVersions("DYLD_IOS_VERSION_", "--ios");
	}
	elsif(m/^\/\/\@WATCHOS_VERSION_DEFS\@$/) {
		expandVersions("DYLD_WATCHOS_VERSION_", "--watchos");
	}
	else {
		print $_;
	}
}

