#include <iostream>
#include <vector>
#include <getopt.h>
#include <functional>
#include <regex>
//#include "intervaltree.hpp"
#include "subcommand.hpp"
#include "stream.hpp"
#include "index.hpp"
#include "position.hpp"
#include "vg.pb.h"
//#include "genotyper.hpp"
#include "genotypekit.hpp"
#include "path_index.hpp"
#include "vg.hpp"
#include "srpe.hpp"
#include "filter.hpp"
#include "utility.hpp"
#include "Variant.h"
#include "translator.hpp"
#include "Fasta.h"

using namespace std;
using namespace vg;
using namespace vg::subcommand;

void help_srpe(char** argv){
    cerr << "Usage: " << argv[0] << " srpe [options] <data.gam> <data.gam.index> <graph.vg>" << endl
        << "Options: " << endl

        << "   -S / --specific <VCF>    look up variants in <VCF> in the graph and report only those." << endl
        << "   -R / --recall            recall (i.e. type) all variants with paths stored in the graph." << endl
        << "   -r / --reference         reference genome to pull structural variants from." << endl
        << "   -I / --insertions        fasta file containing insertion sequences." << endl
        << endl;
    //<< "-S / --SV-TYPE comma separated list of SV types to detect (default: all)." << endl



}



int main_srpe(int argc, char** argv){
    string gam_name = "";
    string gam_index_name = "";
    string graph_name = "";
    string xg_name = "";
    string gcsa_name = "";
    string lcp_name = "";

    string spec_vcf = "";
    string ref_fasta = "";
    string ins_fasta = "";

    int max_iter = 2;
    int max_frag_len = 10000;
    int min_soft_clip = 12;

    bool do_all = false;

    vector<string> search_types;
    search_types.push_back("DEL");

    int threads = 1;

    if (argc <= 2) {
        help_srpe(argv);
        return 1;
    }

    int c;
    optind = 2; // force optind past command positional argument
    while (true) {
        static struct option long_options[] =
        {
            {"max-iter", required_argument, 0, 'm'},
            {"xg-index", required_argument, 0, 'x'},
            {"help", no_argument, 0, 'h'},
            {"gcsa-index", required_argument, 0, 'g'},
            {"specific", required_argument, 0, 'S'},
            {"recall", no_argument, 0, 'R'},
            {"insertions", required_argument, 0, 'I'},
            {"reference", required_argument, 0, 'r'},
            {"threads", required_argument, 0, 't'},
            {0, 0, 0, 0}

        };
        int option_index = 0;
        c = getopt_long (argc, argv, "hx:g:m:S:RI:r:t:",
                long_options, &option_index);

        // Detect the end of the options.
        if (c == -1)
            break;

        switch (c)
        {
            case 'm':
                max_iter = atoi(optarg);
                break;

            case 't':
                threads = atoi(optarg);
                break;

            case 'R':
                do_all = true;
                break;
            case 'x':
                xg_name = optarg;
                break;
            case 'g':
                gcsa_name = optarg;
                break;
            case 'S':
                spec_vcf = optarg;
                break;
            case 'r':
                ref_fasta = optarg;
                break;
            case 'I':
                ins_fasta = optarg;
                break;
            case 'h':
            case '?':
            default:
                help_srpe(argv);
                abort();
        }

    }

    omp_set_num_threads(threads);


    SRPE srpe;


    gam_name = argv[optind];
    //gam_index_name = argv[++optind];
    graph_name = argv[++optind];

    xg::XG* xg_ind = new xg::XG();
    Index gamind;

    vg::VG* graph;

    // if (!xg_name.empty()){
    //     ifstream in(xg_name);
    //     xg_ind->load(in);
    // }
    // if (!gam_index_name.empty()){
    //     gamind.open_read_only(gam_index_name);
    // }
    // else{

    // }

    if (!graph_name.empty()){
        ifstream in(graph_name);
        graph = new VG(in, false);
    }

    // Open a variant call file,
    // hash each variant to an hash ID
    // have in if in the loop below.
    map<string, Locus> name_to_loc;
    // Makes a pathindex, which allows us to query length and push out a VCF with a position
    map<string, PathIndex*> pindexes;
    regex is_alt ("_alt_.*");

    vector<FastaReference*> insertions;
    if (!ins_fasta.empty()){
        FastaReference* ins = new FastaReference();
        insertions.emplace_back(ins);
        ins->open(ins_fasta);

    }

    if (!spec_vcf.empty() && ref_fasta.empty()){
        cerr << "Error: option -S requires a fasta reference using the -r <reference> flag" << endl;
    }
    else if (!spec_vcf.empty()){

        FastaReference* linear_ref = new FastaReference();
        linear_ref->open(ref_fasta);

        for (auto r_path : (graph->paths)._paths){
            if (!regex_match(r_path.first, is_alt)){
                pindexes[r_path.first] = new PathIndex(*graph, r_path.first, true);
            }
        }

        vcflib::VariantCallFile* variant_file = new vcflib::VariantCallFile();
        variant_file->open(spec_vcf);

        string descrip = "";
        descrip = "##INFO=<ID=AD,Number=R,Type=Integer,Description=\"Allele depth for each allele.\"\\>";
        variant_file->addHeaderLine(descrip);

        cout << variant_file->header << endl;

        unordered_map<string, list<Mapping> > graphpaths( (graph->paths)._paths.begin(), (graph->paths)._paths.end() );
        map<string, vcflib::Variant> hash_to_var;
        unordered_map<string, vector<int64_t> > varname_to_nodeid;
        unordered_map<int64_t, int32_t> node_to_depth;
        vector<int64_t> variant_nodes;
        variant_nodes.reserve(10000);

        // Hash a variant from the VCF
        vcflib::Variant var;
        while (variant_file->getNextVariant(var)){
            var.position -= 1;
            var.canonicalize_sv(*linear_ref, insertions, -1);
            string var_id = make_variant_id(var);
            hash_to_var[ var_id ] = var;
            for (int alt_ind = 0; alt_ind <= var.alt.size(); alt_ind++){
                string alt_id = "_alt_" + var_id + "_" + std::to_string(alt_ind);
                list<Mapping> x_path = graphpaths[ alt_id ];
                for (Mapping x_m : x_path){
                    variant_nodes.push_back(x_m.position().node_id());
                    varname_to_nodeid[ alt_id ].push_back(x_m.position().node_id());
                }
            }

        }
        std::function<void(const Alignment& a)> incr = [&](const Alignment& a){
            for (int i = 0; i < a.path().mapping_size(); i++){
                node_to_depth[ a.path().mapping(i).position().node_id() ] += 1;
            }
        };

        gamind.for_alignment_to_nodes(variant_nodes, incr);

        cerr << node_to_depth.size () << " reads in count map" << endl;

        for (auto it : hash_to_var){
            for (int i = 0; i <= it.second.alt.size(); i++){
                int32_t sum_reads = 0;
                string alt_id = "_alt_" + it.first + "_" + std::to_string(i);
                for (int i = 0; i < varname_to_nodeid[ alt_id ].size(); i++){
                    sum_reads += node_to_depth[varname_to_nodeid[ alt_id ][i]];

                }
                #pragma omp critical
                it.second.info["AD"].push_back(std::to_string(sum_reads));
            }
            cout << it.second << endl;
        }

    }
    else if (do_all){
        vector<Support> supports;

        for (auto r_path : (graph->paths)._paths){
            if (!regex_match(r_path.first, is_alt)){
                pindexes[r_path.first] = new PathIndex(*graph, r_path.first, true);
            }
        }
        for (auto x_path : (graph->paths)._paths){
            cerr << x_path.first << endl;
            int32_t support = 0;
            if (regex_match(x_path.first, is_alt)){
                vector<Alignment> alns;
                vector<int64_t> var_node_ids;
                for (Mapping x_m : x_path.second){
                    var_node_ids.push_back(x_m.position().node_id()); 
                }

                std::function<void(const Alignment&)> incr = [&](const Alignment& a){
                    ++support;
                };
                gamind.for_alignment_to_nodes(var_node_ids, incr);
                cout << support << " reads support " << x_path.first << endl;
            }
        }
    }
    else{

        // First, slurp in our discordant, split-read,
        // one-end-anchored, etc reads.
        // basename = * 
        // *.gam.split
        // *.gam.oea
        // *.gam.unmapped
        // *.gam.discordant
        
        ifstream all_reads;
        all_reads.open(gam_name);
        // Reads with no mismatches.
        vector<Alignment> perfects;
        // Reads with anchored mismatches or indels.
        vector<Alignment> simple_mismatches;
        // All the other reads
        vector<Alignment> complex_reads;

        Filter ff;

        vector<Path> simple_paths;

        std::function<void(Alignment&)> get_simples_and_perfects = [&](Alignment& aln){
            if (ff.perfect_filter(aln)){
                #pragma omp critical
                perfects.push_back(aln);
            }
            else if (ff.simple_filter(aln)){
                #pragma omp critical
                {
                    simple_mismatches.push_back(aln);
                    simple_paths.push_back(aln.path());
                }
            }
            // else{
            //     #pragma omp critical
            //     complex_reads.push_back(aln);
            // }
        };

        stream::for_each_parallel(all_reads, get_simples_and_perfects);

        if (true){
            cerr << perfects.size() << " perfect reads." << endl;
            cerr << simple_mismatches.size() << " simple reads." << endl; 
            cerr << complex_reads.size() << " more complex reads." << endl;
        }

        // Grab our perfect and simple reads
        // incorporate our simple reads.

        vector<Translation> translations = graph->edit(simple_paths);
        
        // Add both simple and perfect reads to the depth map.
        DepthMap dm(graph->size());
        dm.fill(simple_mismatches);
        dm.fill(perfects);
        
        // Save memory by disposing of our perfects and simples.
        // TODO is clear enough?
        simple_mismatches.clear();
        perfects.clear();

        // You could call SNPs right here. We should also check known variants at this stage.
        // Call snarlmanager, tossing out trivial snarls
        // Report them as either loci or VCF records
        SnarlFinder* snf = new CactusUltrabubbleFinder(*graph, "", true);

        SnarlManager snarl_manager = snf->find_snarls();
        vector<const Snarl*> snarl_roots = snarl_manager.top_level_snarls();

        TraversalFinder* trav_finder = new ExhaustiveTraversalFinder(*graph, snarl_manager);

        bool justSNPs = true;
        if (justSNPs){
            for (auto x : snarl_roots){
                vector<SnarlTraversal> site_traversals = trav_finder->find_traversals(*x);
                for (auto t : site_traversals){
                    //cout << t.start() << " ";
                    for (auto zed : t.visits()){
                        cout << zed.node_id() << ", ";
                    }
                    cout <<  endl;
                }
            }
        }

        // Read in all our nasty discordant/split/clipped reads and
        // filter the nice ones from the nasty ones.

        double expected_insert_size = 500.0;
        double expected_insert_sd = 100.0;

        std::unordered_map<string, Alignment> mates_by_name;
        std::function<void(pair<Alignment, Alignment>)> make_mate_map = [&](pair<Alignment, Alignment> alns){
            if (alns.second.name() != ""){
                mates_by_name[alns.second.name()] = alns.second;
            }
            else if (warned){

            }
            else{
                cerr << "Alignment with no name present." << endl;
                warned = true;
            }
        };
        stream::for_each_interleaved_pair_parallel(all_reads, make_mate_map);

        std::function<void(Alignment&)> rectify_single_read = [&](Alignment& a){

        };

        std::function<void(pair<Alignment, Alignment>)> rectify_read_pair = [&](pair<Alignment, Alignment> alns){

        };

        // for every read in our nasty set, try to normalize it. If we succeed,
        // grab its mate (if appropriate) and generate an IMPRECISE variant based on the two.
        // Place that IMPRECISE candidate somewhere within its predicted range in the graph.

        // If desired, remap our unmapped and nasty reads. This will help us to augment our depth map.


        // call variants 
    
    }


    return 0;
}

static Subcommand vg_srpe ("srpe", "graph-external SV detection", main_srpe);

