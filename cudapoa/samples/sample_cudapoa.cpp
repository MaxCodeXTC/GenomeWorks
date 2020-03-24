/*
* Copyright (c) 2019, NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto.  Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#include "../benchmarks/common/utils.hpp"

#include <file_location.hpp>
#include <claragenomics/cudapoa/cudapoa.hpp>
#include <claragenomics/cudapoa/batch.hpp>
#include <claragenomics/utils/signed_integer_utils.hpp>
#include <claragenomics/utils/cudautils.hpp>

#include <cuda_runtime_api.h>
#include <vector>
#include <string>
#include <stdexcept>
#include <unistd.h>
#include <random>
#include <claragenomics/utils/genomeutils.hpp>

using namespace claragenomics;
using namespace claragenomics::cudapoa;

std::unique_ptr<Batch> initialize_batch(bool msa, BatchSize batch_size)
{
    // Get device information.
    int32_t device_count = 0;
    CGA_CU_CHECK_ERR(cudaGetDeviceCount(&device_count));
    assert(device_count > 0);

    size_t total = 0, free = 0;
    cudaSetDevice(0); // Using first GPU for sample.
    cudaMemGetInfo(&free, &total);

    // Initialize internal logging framework.
    Init();

    // Initialize CUDAPOA batch object for batched processing of POAs on the GPU.
    const int32_t device_id                   = 0;
    cudaStream_t stream                       = 0;
    size_t mem_per_batch                      = 0.9 * free; // Using 90% of GPU available memory for CUDAPOA batch.
    const int32_t mismatch_score = -6, gap_score = -8, match_score = 8;
    bool banded_alignment = false;

    std::unique_ptr<Batch> batch = create_batch(device_id,
                                                stream,
                                                mem_per_batch,
                                                msa ? OutputType::msa : OutputType::consensus,
                                                batch_size,
                                                gap_score,
                                                mismatch_score,
                                                match_score,
                                                banded_alignment);

    return std::move(batch);
}

void process_batch(Batch* batch, bool msa, bool print)
{
    batch->generate_poa();

    StatusType status = StatusType::success;
    if (msa)
    {
        // Grab MSA results for all POA groups in batch.
        std::vector<std::vector<std::string>> msa; // MSA per group
        std::vector<StatusType> output_status;     // Status of MSA generation per group

        status = batch->get_msa(msa, output_status);
        if (status != StatusType::success)
        {
            std::cerr << "Could not generate MSA for batch : " << status << std::endl;
        }

        for (int32_t g = 0; g < get_size(msa); g++)
        {
            if (output_status[g] != StatusType::success)
            {
                std::cerr << "Error generating  MSA for POA group " << g << ". Error type " << output_status[g] << std::endl;
            }
            else
            {
                if (print)
                {
                    for (const auto& alignment : msa[g])
                    {
                        std::cout << alignment << std::endl;
                    }
                }
            }
        }
    }
    else
    {
        // Grab consensus results for all POA groups in batch.
        std::vector<std::string> consensus;          // Consensus string for each POA group
        std::vector<std::vector<uint16_t>> coverage; // Per base coverage for each consensus
        std::vector<StatusType> output_status;       // Status of consensus generation per group

        status = batch->get_consensus(consensus, coverage, output_status);
        if (status != StatusType::success)
        {
            std::cerr << "Could not generate consensus for batch : " << status << std::endl;
        }

        for (int32_t g = 0; g < get_size(consensus); g++)
        {
            if (output_status[g] != StatusType::success)
            {
                std::cerr << "Error generating consensus for POA group " << g << ". Error type " << output_status[g] << std::endl;
            }
            else
            {
                if (print)
                {
                    std::cout << consensus[g] << std::endl;
                }
            }
        }
    }
}

void sample_long_reads(bool msa, bool print)
{
    constexpr uint32_t random_seed = 5827349;
    std::minstd_rand rng(random_seed);

    const int16_t number_of_reads = 2;
    const int32_t read_length     = 1000;
    int32_t max_sequence_length   = read_length + 1;

    std::vector<std::pair<int, int>> variation_ranges;
    //variation_ranges.push_back(std::pair<int, int>(3, 5));
    //variation_ranges.push_back(std::pair<int, int>(300, 500));
    //variation_ranges.push_back(std::pair<int, int>(1000, 1300));
    //variation_ranges.push_back(std::pair<int, int>(2000, 2200));
    //variation_ranges.push_back(std::pair<int, int>(3000, 3500));
    //variation_ranges.push_back(std::pair<int, int>(4000, 4200));

    std::vector<std::string> long_reads(number_of_reads);
    long_reads[0] = claragenomics::genomeutils::generate_random_genome(read_length, rng);
    for (size_t i = 1; i < long_reads.size(); i++)
    {
        long_reads[i]       = claragenomics::genomeutils::generate_random_sequence(long_reads[0], rng, read_length, read_length, read_length, &variation_ranges);
        max_sequence_length = max_sequence_length > get_size(long_reads[i]) ? max_sequence_length : get_size(long_reads[i]) + 1;
    }

    // Define upper limits for sequence size, graph size ....
    BatchSize batch_size;
    batch_size.setSize(max_sequence_length, 100);

    // Initialize batch.
    std::unique_ptr<Batch> batch = initialize_batch(msa, batch_size);

    Group poa_group;
    // Create a new entry for each sequence and add to the group.
    for (const auto& read : long_reads)
    {
        Entry poa_entry{};
        poa_entry.seq     = read.c_str();
        poa_entry.length  = read.length();
        poa_entry.weights = nullptr;
        poa_group.push_back(poa_entry);
    }

    std::vector<StatusType> read_status;
    StatusType status = batch->add_poa_group(read_status, poa_group);

    if (status == StatusType::success)
    {
        // Check if all reads in POA group were added successfully.
        for (const auto& s : read_status)
        {
            if (s == StatusType::exceeded_maximum_sequence_size)
            {
                std::cerr << "Dropping sequence because sequence exceeded maximum size" << std::endl;
            }
        }
    }

    if (status != StatusType::exceeded_maximum_poas && status != StatusType::success)
    {
        std::cerr << "Could not add POA group to batch. Error code " << status << std::endl;
    }
    else
    {
        // Now process batch.
        process_batch(batch.get(), msa, print);
    }

    std::vector<DirectedGraph> graph;
    std::vector<StatusType> graph_status;

    batch->get_graphs(graph, graph_status);

    for (const auto& s : graph_status)
    {
        if (s != StatusType::success)
        {
            std::cerr << "Failed to process long-read batch. Error code " << s << std::endl;
        }
        else
        {
            std::cerr << "Processed long-read batch successfully." << std::endl;
        }
    }

    //std::string graph_dot = graph.front().serialize_to_dot();
}

int main(int argc, char** argv)
{
    // Process options
    int c          = 0;
    bool msa       = false;
    bool long_read = false;
    bool help      = false;
    bool print     = false;

    while ((c = getopt(argc, argv, "mlhp")) != -1)
    {
        switch (c)
        {
        case 'm':
            msa = true;
            break;
        case 'l':
            long_read = true;
            break;
        case 'p':
            print = true;
            break;
        case 'h':
            help = true;
            break;
        }
    }

    if (help)
    {
        std::cout << "CUDAPOA API sample program. Runs consensus or MSA generation on pre-canned data." << std::endl;
        std::cout << "Usage:" << std::endl;
        std::cout << "./sample_cudapoa [-m] [-h]" << std::endl;
        std::cout << "-m : Generate MSA (if not provided, generates consensus by default)" << std::endl;
        std::cout << "-l : Perform long-read sample (if not provided, will run window-based sample by default)" << std::endl;
        std::cout << "-p : Print the MSA or consensus output to stdout" << std::endl;
        std::cout << "-h : Print help message" << std::endl;
        std::exit(0);
    }

    if (long_read)
    {
        sample_long_reads(msa, print);
        return 0;
    }

    // Load input data. Each POA group is represented as a vector of strings. The sample
    // data has many such POA groups to process, hence the data is loaded into a vector
    // of vector of strings.
    const std::string input_data = std::string(CUDAPOA_BENCHMARK_DATA_DIR) + "/sample-windows.txt";
    std::vector<std::vector<std::string>> windows;
    parse_window_data_file(windows, input_data, 1000); // Generate windows.
    assert(get_size(windows) > 0);

    // Define upper limits for sequence size, graph size ....
    BatchSize batch_size;
    batch_size.setSize(1024, 100);

    // Initialize batch.
    std::unique_ptr<Batch> batch = initialize_batch(msa, batch_size);

    // Loop over all the POA groups, add them to the batch and process them.
    int32_t window_count = 0;
    // to avoid potential infinite loop
    int32_t error_count = 0;
    for (int32_t i = 0; i < get_size(windows);)
    {
        const std::vector<std::string>& window = windows[i];

        Group poa_group;
        // Create a new entry for each sequence and add to the group.
        for (const auto& seq : window)
        {
            Entry poa_entry{};
            poa_entry.seq     = seq.c_str();
            poa_entry.length  = seq.length();
            poa_entry.weights = nullptr;
            poa_group.push_back(poa_entry);
        }

        std::vector<StatusType> seq_status;
        StatusType status = batch->add_poa_group(seq_status, poa_group);

        if (status == StatusType::success)
        {
            // Check if all sequences in POA group wre added successfully.
            for (const auto& s : seq_status)
            {
                if (s == StatusType::exceeded_maximum_sequence_size)
                {
                    std::cerr << "Dropping sequence because sequence exceeded maximum size" << std::endl;
                }
            }
            i++;
        }
        // NOTE: If number of windows smaller than batch capacity, then run POA generation
        // once last window is added to batch.
        if (status == StatusType::exceeded_maximum_poas || (i == get_size(windows) - 1))
        {
            // No more POA groups can be added to batch. Now process batch.
            process_batch(batch.get(), msa, print);

            // After MSA is generated for batch, reset batch to make roomf or next set of POA groups.
            batch->reset();

            std::cout << "Processed windows " << window_count << " - " << i << std::endl;
            window_count = i;
        }

        if (status != StatusType::exceeded_maximum_poas && status != StatusType::success)
        {
            std::cerr << "Could not add POA group to batch. Error code " << status << std::endl;
            error_count++;
            if (error_count > get_size(windows))
                break;
        }
    }

    return 0;
}
