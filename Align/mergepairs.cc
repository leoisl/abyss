#include "smith_waterman.h"
#include "config.h"
#include "DataLayer/Options.h"
#include "Align/Options.h"
#include "Common/Options.h"
#include "FastaReader.h"
#include "IOUtil.h"
#include "Uncompress.h"
#include "alignGlobal.h"
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <getopt.h>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace std;

#define PROGRAM "abyss-align"

static const char VERSION_MESSAGE[] =
PROGRAM " (" PACKAGE_NAME ") " VERSION "\n"
"Written by Anthony Raymond.\n"
"\n"
"Copyright 2012 Canada's Michael Smith Genome Science Centre\n";

static const char USAGE_MESSAGE[] =
"Usage: " PROGRAM " [OPTION]... READS1 READS2\n"
"Attempt to merge reads in READS1 with reads in READS2\n"
"\n"
" Options:\n"
"  -o, --prefix=PREFIX     the prefix of all output files [out]\n"
"  -p, --identity=N        minimum overlap identity [0.9]\n"
"  -m, --matches=N         minimum number of matches in overlap [10]\n"
"  -l, --length=N          trim bases from 3' end of reads until\n"
"                          reads are a maximum of N bp long [0]\n"
"      --chastity          discard unchaste reads [default]\n"
"      --no-chastity       do not discard unchaste reads\n"
"      --trim-masked       trim masked bases from the ends of reads\n"
"      --no-trim-masked    do not trim masked bases from the ends\n"
"                          of reads [default]\n"
"  -q, --trim-quality=N    trim bases from the ends of reads whose\n"
"                          quality is less than the threshold\n"
"      --standard-quality  zero quality is `!' (33)\n"
"                          default for FASTQ and SAM files\n"
"      --illumina-quality  zero quality is `@' (64)\n"
"                          default for qseq and export files\n"
"  -v, --verbose           display verbose output\n"
"      --help              display this help and exit\n"
"      --version           output version information and exit\n"
"\n"
"Report bugs to <" PACKAGE_BUGREPORT ">.\n";

namespace opt {
	static string prefix = "out";
	static float identity = 0.9;
	static unsigned min_matches = 10;
}

static struct {
	unsigned total_reads;
	unsigned merged_reads;
	unsigned unmerged_reads;
	unsigned no_alignment;
	unsigned too_many_aligns;
	unsigned low_matches;
	unsigned has_indel;
	unsigned pid_low;
} stats;

static const char shortopts[] = "o:p:m:q:l:v";

enum { OPT_HELP = 1, OPT_VERSION };

static const struct option longopts[] = {
	{ "prefix",           required_argument, NULL, 'o' },
	{ "identity",         required_argument, NULL, 'p' },
	{ "matches",          required_argument, NULL, 'm' },
	{ "verbose",          no_argument,       NULL, 'v' },
	{ "length",           no_argument,       NULL, 'l' },
	{ "chastity",         no_argument,       &opt::chastityFilter, 1 },
	{ "no-chastity",      no_argument,       &opt::chastityFilter, 0 },
	{ "trim-masked",      no_argument,       &opt::trimMasked, 1 },
	{ "no-trim-masked",   no_argument,       &opt::trimMasked, 0 },
	{ "trim-quality",     required_argument, NULL, 'q' },
	{ "standard-quality", no_argument,       &opt::qualityOffset, 33 },
	{ "illumina-quality", no_argument,       &opt::qualityOffset, 64 },
	{ "help",             no_argument,       NULL, OPT_HELP },
	{ "version",          no_argument,       NULL, OPT_VERSION },
	{ NULL, 0, NULL, 0 }
};

char bestBase(char a, char b, char qa, char qb) {
	return qa > qb ? a : b;
}

/** Merge the read sequences taking the highest quality base when
 * there is a dissagreement while reporting the lowest quality
 * possible. */
static void mergeReads(overlap_align& overlap, FastqRecord& rec1,
		FastqRecord& rec2, FastqRecord& out)
{
	size_t ol = overlap.length();

	Sequence seq1 = rec1.seq;
	string qual1 = rec1.qual;

	Sequence rc_seq2 = reverseComplement(rec2.seq);
	string rc_qual2(rec2.qual);
	reverse(rc_qual2.begin(), rc_qual2.end());

	// Form overhanging portions of the reads.
	size_t out_len = overlap.overlap_t_pos + ol + rc_seq2.length() -
		overlap.overlap_h_pos - 1;
	Sequence out_seq(out_len, 'N');
	string out_qual(out_len, '#');
	assert(out_seq.length() >= seq1.length() || out_seq.length() >=
			rc_seq2.length());

	copy(seq1.begin(), seq1.begin() + overlap.overlap_t_pos,
			out_seq.begin());
	copy(rc_seq2.begin() + overlap.overlap_h_pos + 1, rc_seq2.end(),
			out_seq.begin() + overlap.overlap_t_pos + ol);
	copy(qual1.begin(), qual1.begin() + overlap.overlap_t_pos,
			out_qual.begin());
	copy(rc_qual2.begin() + ol, rc_qual2.end(),
			out_qual.begin() + seq1.length());

	// Fix the sequence and quality inside the overlap.
	for (unsigned i = 0; i < ol; i++) {
		assert(int(seq1.length() - ol + i) >= 0);
		unsigned pos = seq1.length() - ol + i;
		assert(pos < seq1.length() && i < rc_seq2.length() &&
				pos < out_seq.length());
		if (seq1[pos] == rc_seq2[i]) {
			out_seq[pos] = seq1[pos];
			out_qual[pos] = max(qual1[pos], rc_qual2[i]);
		} else {
			out_seq[pos] = bestBase(seq1[pos], rc_seq2[i],
					qual1[pos], rc_qual2[i]);
			out_qual[pos] = min(qual1[pos], rc_qual2[i]);
		}
	}
	//cout << seq1 << '\n' << rc_seq2 << '\n' << out_seq << '\n';
	//cout << qual1 << '\n' << rc_qual2 << '\n' << out_qual << '\n';
	out = FastqRecord(rec1.id, rec1.comment, out_seq, out_qual);
}

bool isGapless(overlap_align& o, Sequence& s) {
	return o.length() == s.length() - o.overlap_t_pos &&
		o.length() == o.overlap_h_pos + 1;
}

static void filterAlignments(vector<overlap_align>& overlaps,
		FastaRecord& rec)
{
	if (overlaps.empty()) {
		stats.no_alignment++;
		return;
	}

	vector<overlap_align>::iterator it;
	for (it = overlaps.begin(); it != overlaps.end(); it++ ) {
		overlap_align o = *it;
		if (o.overlap_match < opt::min_matches)
			overlaps.erase(it--);
	}
	if (overlaps.empty()) {
		stats.low_matches++;
		return;
	}

	for (it = overlaps.begin(); it != overlaps.end(); it++ ) {
		overlap_align o = *it;
		if (o.pid() < opt::identity)
			overlaps.erase(it--);
	}
	if (overlaps.empty()) {
		stats.pid_low++;
		return;
	}

	for (it = overlaps.begin(); it != overlaps.end(); it++ ) {
		overlap_align o = *it;
		if (!isGapless(o, rec.seq))
			overlaps.erase(it--);
	}
	if (overlaps.empty()) {
		stats.has_indel++;
		return;
	}
}

/** Align read pairs. */
static void alignFiles(const char* reads1, const char* reads2)
{
	if (opt::verbose > 0)
		cerr << "Merging `" << reads1 << "' with `" << reads2 << "'\n";
	FastaReader r1(reads1, FastaReader::NO_FOLD_CASE);
	FastaReader r2(reads2, FastaReader::NO_FOLD_CASE);

	// Openning the output files
	string name(opt::prefix);
	name.append("_reads_1.fastq");
	ofstream unmerged1(name.c_str());
	name = string(opt::prefix);
	name.append("_reads_2.fastq");
	ofstream unmerged2(name.c_str());
	name = string(opt::prefix);
	name.append("_merged.fastq");
	ofstream merged(name.c_str());

	FastqRecord rec1, rec2;
	int x = 0;
	while (r1 >> rec1 && r2 >> rec2) {
		stats.total_reads++;
		string rc_qual2(rec2.qual);
		reverse(rc_qual2.begin(), rc_qual2.end());
		vector<overlap_align> overlaps;
		alignOverlap(rec1.seq, reverseComplement(rec2.seq), 0, overlaps,
				true, opt::verbose > 2);

		filterAlignments(overlaps, rec1);

		if (overlaps.size() == 1) {
			// If there is only one good alignment, merge reads and
			// print to merged file
			stats.merged_reads++;
			FastqRecord out;
			mergeReads(overlaps[0], rec1, rec2, out);
			merged << out;
			cout << overlaps[0].length() << ' ' <<
				overlaps[0].overlap_match << '\n';
		} else {
			// print reads to separate files
			if (overlaps.size() > 1)
				stats.too_many_aligns++;
			stats.unmerged_reads++;
			unmerged1 << rec1;
			unmerged2 << rec2;
		}
		if (opt::verbose > 0 && ++x % 10000 == 0) {
			cerr << "Aligned " << x << " reads.\n";
		}
	}
	r2 >> rec2;
	assert(r1.eof());
	assert(r2.eof());
	unmerged1.close();
	unmerged2.close();
	merged.close();
}

int main(int argc, char** argv)
{
	bool die = false;

	//defaults for alignment parameters
	opt::match = 1;
	opt::mismatch = -2;
	opt::gap_open = -10000;
	opt::gap_extend = -10000;

	for (int c; (c = getopt_long(argc, argv,
					shortopts, longopts, NULL)) != -1;) {
		istringstream arg(optarg != NULL ? optarg : "");
		switch (c) {
			case '?': die = true; break;
			case 'o': arg >> opt::prefix; break;
			case 'p': arg >> opt::identity; break;
			case 'm': arg >> opt::min_matches; break;
			case 'q': arg >> opt::qualityThreshold; break;
			case 'l': arg >> opt::maxLength; break;
			case 'v': opt::verbose++; break;
			case OPT_HELP:
					  cerr << USAGE_MESSAGE;
					  exit(EXIT_SUCCESS);
			case OPT_VERSION:
					  cerr << VERSION_MESSAGE;
					  exit(EXIT_SUCCESS);
		}
		if (optarg != NULL && !arg.eof()) {
			cerr << PROGRAM ": invalid option: `-"
				<< (char)c << optarg << "'\n";
			exit(EXIT_FAILURE);
		}
	}

	if (argc - optind < 2) {
		cerr << PROGRAM ": missing arguments\n";
		die = true;
	}

	if (argc - optind > 2) {
		cerr << PROGRAM ": too many arguments\n";
		die = true;
	}

	if (die) {
		cerr << "Try `" << PROGRAM
			<< " --help' for more information.\n";
		exit(EXIT_FAILURE);
	}

	const char* reads1 = argv[optind++];
	const char* reads2 = argv[optind++];

	alignFiles(reads1, reads2);

	cerr << "Read merging stats: total=" << stats.total_reads
		<< " merged=" << stats.merged_reads
		<< " unmerged=" << stats.unmerged_reads << '\n'
		<< "no_alignment=" << stats.no_alignment
		<< " too_many_aligns=" << stats.too_many_aligns
		<< " too_few_matches=" << stats.low_matches
		<< " has_indel=" << stats.has_indel
		<< " low_pid=" << stats.pid_low << '\n';

	return 0;
}
