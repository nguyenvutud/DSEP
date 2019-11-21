// Copyright Steinwurf ApS 2011.
// Distributed under the "STEINWURF RESEARCH LICENSE 1.0".
// See accompanying file LICENSE.rst or
// http://www.steinwurf.com/licensing

#pragma once

#include <cstdint>
#include <cassert>
#include <vector>
#include <string>
#include <type_traits>

#include <gauge/gauge.hpp>
#include <tables/table.hpp>

#include <boost/random/bernoulli_distribution.hpp>
#include <boost/random/mersenne_twister.hpp>

#include <fifi/is_prime2325.hpp>
#include <fifi/prime2325_binary_search.hpp>
#include <fifi/prime2325_apply_prefix.hpp>

#include <kodo_core/has_deep_symbol_storage.hpp>
#include <kodo_core/has_set_mutable_symbols.hpp>
#include <kodo_core/set_systematic_off.hpp>
#include <kodo_core/set_mutable_symbols.hpp>
#include <kodo_core/read_payloads.hpp>
#include <kodo_core/write_payloads.hpp>

/// Tag to turn on block coding in the benchmark
struct block_coding_on {};

/// Tag to turn off block coding in the benchmark
struct block_coding_off {};

/// A test block represents an encoder and decoder pair
template<class Encoder, class Decoder, class BlockCoding = block_coding_off>
struct sparse_tunable_throughput_benchmark : public gauge::time_benchmark
{
    using encoder_factory = typename Encoder::factory;
    using encoder_ptr = typename Encoder::factory::pointer;

    using decoder_factory = typename Decoder::factory;
    using decoder_ptr = typename Decoder::factory::pointer;
    sparse_tunable_throughput_benchmark(){

        // Seed the random generator controlling the erasures
        m_random_generator.seed((uint32_t)time(0));
    }
    void init()
    {
        m_factor = 1;
        gauge::time_benchmark::init();
    }

    void start()
    {
        m_encoded_symbols = 0;
 
        m_recovered_symbols = 0;
        m_processed_symbols = 0;
        gauge::time_benchmark::start();
    }

    void stop()
    {
        gauge::time_benchmark::stop();
    }

    double calculate_throughput(bool goodput = false)
    {
        // Get the time spent per iteration
        m_time = gauge::time_benchmark::measurement();

        gauge::config_set cs = get_current_configuration();
        std::string type = cs.get_value<std::string>("type");
        //uint32_t symbols = cs.get_value<uint32_t>("symbols");
        uint32_t symbol_size = cs.get_value<uint32_t>("symbol_size");

        // The number of bytes {en|de}coded
        uint64_t total_bytes = 0;

        if (type == "decoder")
        {
            if (goodput)
                total_bytes = m_recovered_symbols * symbol_size;
            else
                total_bytes = m_processed_symbols * symbol_size;
        }
        else if (type == "encoder")
        {
            total_bytes = m_encoded_symbols * symbol_size;
        }
        else
        {
            assert(0 && "Unknown benchmark type");
        }

        // The bytes per iteration
        uint64_t bytes =
            total_bytes / gauge::time_benchmark::iteration_count();

        return bytes / m_time; // MB/s for each iteration
    }

    void store_run(tables::table& results)
    {
        if (!results.has_column("throughput"))
            results.add_column("throughput");

        if (!results.has_column("goodput"))
            results.add_column("goodput");

        results.set_value("throughput", calculate_throughput(false));
        results.set_value("goodput", calculate_throughput(true));
	
	 if (!results.has_column("used"))
            results.add_column("used");
        results.set_value("used", m_used);
//        if (!results.has_column("time"))
//                   results.add_column("time");
//               results.set_value("time", m_time);

    }

    bool accept_measurement()
    {
        gauge::config_set cs = get_current_configuration();

        std::string type = cs.get_value<std::string>("type");

        if (type == "decoder")
        {
            // If we are benchmarking a decoder we only accept
            // the measurement if the decoding was successful
            if (!m_decoder->is_complete())
            {
                // We did not generate enough payloads to decode successfully,
                // so we will generate more payloads for next run
                ++m_factor;

                return false;
            }

            // At this point, the output data should be equal to the input
            // data
            assert(m_data_out == m_data_in);
        }

        return gauge::time_benchmark::accept_measurement();
    }

    std::string unit_text() const
    {
        return "MB/s";
    }

    void get_options(gauge::po::variables_map& options)
    {
        auto symbols = options["symbols"].as<std::vector<uint32_t>>();
        auto symbol_size = options["symbol_size"].as<std::vector<uint32_t>>();
        auto types = options["type"].as<std::vector<std::string>>();
        auto expansion = options["expansion"].as<std::vector<uint32_t> >();
        auto erasure = options["erasure"].as<std::vector<double> >();

        auto threshold = options["threshold"].as<std::vector<uint32_t> >();

        assert(symbols.size() > 0);
        assert(symbol_size.size() > 0);
        assert(types.size() > 0);
        assert(expansion.size() > 0);

        assert(erasure.size() > 0);

        for (const auto& s : symbols)
        {
            for (const auto& p : symbol_size)
            {
                for (const auto& t : types)
                {

                    for (const auto& e: expansion)
                    {
					   for (const auto& er: erasure)
						{
						    for (const auto& th: threshold)
						    {
								gauge::config_set cs;
								cs.set_value<uint32_t>("symbols", s);
								cs.set_value<uint32_t>("symbol_size", p);
								cs.set_value<std::string>("type", t);
								cs.set_value<uint32_t>("expansion", e);
								cs.set_value<uint32_t>("threshold", th);

								cs.set_value<double>("erasure", er);

								add_configuration(cs);
						    }
						}
                    }

                }
            }
        }
    }

    void setup()
    {
    	gauge::config_set cs = get_current_configuration();

		uint32_t symbols = cs.get_value<uint32_t>("symbols");
		uint32_t symbol_size = cs.get_value<uint32_t>("symbol_size");

//		double density = cs.get_value<double>("density");

		m_decoder_factory = std::make_shared<decoder_factory>(
			symbols, symbol_size);

		m_encoder_factory = std::make_shared<encoder_factory>(
			symbols, symbol_size);


		m_decoder_factory->set_symbols(symbols);
		m_decoder_factory->set_symbol_size(symbol_size);

		m_encoder_factory->set_symbols(symbols);
		m_encoder_factory->set_symbol_size(symbol_size);


		setup_factories(); //Nvu: add

		m_encoder = m_encoder_factory->build();
//        m_encoder->nested()->set_density(density);

		m_decoder = m_decoder_factory->build();

		m_payload_buffer.resize(m_encoder->payload_size(), 0);

		m_data_in.resize(m_encoder->block_size());

	    std::generate_n(m_data_in.begin(), m_data_in.size(), rand);

		m_encoder->set_const_symbols(storage::storage(m_data_in));

		m_data_out.resize(m_encoder->block_size());

		// Make sure that the encoder and decoder payload sizes are equal
		assert(m_encoder->payload_size() == m_decoder->payload_size());

		// Create the payload buffer
		uint32_t payload_size = m_encoder->payload_size();

		m_temp_payload.resize(payload_size);

		// Prepare storage to the encoded payloads
		uint32_t payload_count = symbols * m_factor;

		assert(payload_count > 0);

		// Allocate contiguous payload buffer and store payload pointers
		m_payload_buffer.resize(payload_count * payload_size);
		m_payloads.resize(payload_count);

		for (uint32_t i = 0; i < payload_count; ++i)
		{
			m_payloads[i] = &m_payload_buffer[i * payload_size];
		}

		m_used = 0;
    }

    /// Setup the factories - we re-factored this into its own function
    /// since some of the codecs like sparse, perpetual and fulcrum
    /// customize the setup of the factory.
    void setup_factories()
      {
          //Super::setup_factories();
          gauge::config_set cs = get_current_configuration();

          uint32_t expansion = cs.get_value<uint32_t>("expansion");

          m_decoder_factory->set_expansion(expansion);
          m_encoder_factory->set_expansion(expansion);


      }

    void encode_payloads_fulcrum(uint32_t symbols, uint32_t expansion)
    {
        // If we are working in the prime2325 finite field we have to
        // "map" the data first. This cost is included in the encoder
        // throughput (we do it with the clock running), just as it
        // would be in a real application.
        if (fifi::is_prime2325<typename Encoder::field_type>::value)
        {
            uint32_t block_length =
                fifi::size_to_length<fifi::prime2325>(m_encoder->block_size());

            fifi::prime2325_binary_search search(block_length);
            m_prefix = search.find_prefix(storage::storage(m_data_in));

            // Apply the negated prefix
            fifi::apply_prefix(storage::storage(m_data_in), ~m_prefix);
        }

        m_encoder->set_const_symbols(storage::storage(m_data_in));

        //use for normal fulcrum decoder
       m_encoder->nested()->set_num_nonzero_expansion(expansion, expansion, false, false, true);

        // We switch any systematic operations off so we code
        // symbols from the beginning
        if (kodo_core::has_set_systematic_off<Encoder>::value)
            kodo_core::set_systematic_off(*m_encoder);

        if (std::is_same<BlockCoding, block_coding_on>::value &&
            kodo_core::has_write_payloads<Encoder>::value)
        {
            m_encoded_symbols +=
                kodo_core::write_payloads(
                    *m_encoder, m_payloads.data(), m_payloads.size());
        }
        else
        {
            for (uint8_t* payload : m_payloads)
            {
            	//aim to set the same extra packets to other schemes
//            	if (m_encoded_symbols >= symbols)
//					m_encoder->nested()->set_num_nonzero_expansion(expansion, expansion, false, true);

                m_encoder->write_payload(payload);
                ++m_encoded_symbols;
            }
        }

        /// @todo Revert to the original input data by re-applying the
        /// prefix to the input data. This is needed since the
        /// benchmark loops and re-encodes the same data. If we did
        /// not re-apply the prefix the input data would be corrupted
        /// in the second iteration, causing decoding to produce
        /// incorrect output data. In a real-world application this
        /// would typically not be needed. For this reason the
        /// prime2325 results encoding results will show a lower
        /// throughput than what we can expect to see in an actual
        /// application. Currently we don't have a good solution for
        /// this. Possible solutions would be to copy the input data,
        /// however in that way we will also see a performance hit.
        if (fifi::is_prime2325<typename Encoder::field_type>::value)
        {
            // Apply the negated prefix
            fifi::apply_prefix(storage::storage(m_data_in), ~m_prefix);
        }
    }
    double calculate_region(uint32_t symbols, uint32_t num_regions)
    {
    	return symbols * (pow(2, num_regions) - 1)/ pow(2, num_regions);
    }


    void encode_payloads_DTEP(uint32_t symbols, uint32_t expansion)
        {
            // If we are working in the prime2325 finite field we have to
            // "map" the data first. This cost is included in the encoder
            // throughput (we do it with the clock running), just as it
            // would be in a real application.
            if (fifi::is_prime2325<typename Encoder::field_type>::value)
            {
                uint32_t block_length =
                    fifi::size_to_length<fifi::prime2325>(m_encoder->block_size());

                fifi::prime2325_binary_search search(block_length);
                m_prefix = search.find_prefix(storage::storage(m_data_in));

                // Apply the negated prefix
                fifi::apply_prefix(storage::storage(m_data_in), ~m_prefix);
            }

            m_encoder->set_const_symbols(storage::storage(m_data_in));

            // We switch any systematic operations off so we code
            // symbols from the beginning
            if (kodo_core::has_set_systematic_off<Encoder>::value)
                kodo_core::set_systematic_off(*m_encoder);

            if (std::is_same<BlockCoding, block_coding_on>::value &&
                kodo_core::has_write_payloads<Encoder>::value)
            {
                m_encoded_symbols +=
                    kodo_core::write_payloads(
                        *m_encoder, m_payloads.data(), m_payloads.size());
            }
            else
            {
            	uint32_t cur_r = 0, received_symbols = 0;
				num_regions = 1;
				double size_cur_region = 0.0;
				m_encoder->nested()->set_num_nonzero_expansion(cur_r, expansion);

				size_cur_region = std::min(double(symbols-expansion + cur_r),
						calculate_region(symbols, num_regions));

				for (uint8_t* payload : m_payloads)
                {
					if ((m_encoded_symbols >= size_cur_region) && (cur_r < expansion))
					{


						if(cur_r < expansion)
						{
							cur_r += 1;
							m_encoder->nested()->set_num_nonzero_expansion(cur_r, expansion, true);
	//						std::cout<<"introduce new coefficient:"<<cur_r<<std::endl;
	//						std::cout<<"current coded packets:"<<m_encoded_symbols<<std::endl;

						}
						num_regions += 1;
						size_cur_region = std::min(double(symbols-expansion + cur_r),
								calculate_region(symbols, num_regions));
						//std::cout<<"new region:"<<num_regions<<" with size:"<<size_cur_region<<std::endl;


					}

//					if (m_encoded_symbols >= symbols)
//						m_encoder->nested()->set_num_nonzero_expansion(cur_r, expansion, false, true);

                    ++m_encoded_symbols;

                    m_encoder->write_payload(payload);
                }
            }

            /// @todo Revert to the original input data by re-applying the
            /// prefix to the input data. This is needed since the
            /// benchmark loops and re-encodes the same data. If we did
            /// not re-apply the prefix the input data would be corrupted
            /// in the second iteration, causing decoding to produce
            /// incorrect output data. In a real-world application this
            /// would typically not be needed. For this reason the
            /// prime2325 results encoding results will show a lower
            /// throughput than what we can expect to see in an actual
            /// application. Currently we don't have a good solution for
            /// this. Possible solutions would be to copy the input data,
            /// however in that way we will also see a performance hit.
            if (fifi::is_prime2325<typename Encoder::field_type>::value)
            {
                // Apply the negated prefix
                fifi::apply_prefix(storage::storage(m_data_in), ~m_prefix);
            }
        }

    void encode_payloads_STEP(uint32_t symbols, uint32_t expansion, uint32_t threshold)
        {
            // If we are working in the prime2325 finite field we have to
            // "map" the data first. This cost is included in the encoder
            // throughput (we do it with the clock running), just as it
            // would be in a real application.
            if (fifi::is_prime2325<typename Encoder::field_type>::value)
            {
                uint32_t block_length =
                    fifi::size_to_length<fifi::prime2325>(m_encoder->block_size());

                fifi::prime2325_binary_search search(block_length);
                m_prefix = search.find_prefix(storage::storage(m_data_in));

                // Apply the negated prefix
                fifi::apply_prefix(storage::storage(m_data_in), ~m_prefix);
            }

            m_encoder->set_const_symbols(storage::storage(m_data_in));

            // We switch any systematic operations off so we code
            // symbols from the beginning
            if (kodo_core::has_set_systematic_off<Encoder>::value)
                kodo_core::set_systematic_off(*m_encoder);

            if (std::is_same<BlockCoding, block_coding_on>::value &&
                kodo_core::has_write_payloads<Encoder>::value)
            {
                m_encoded_symbols +=
                    kodo_core::write_payloads(
                        *m_encoder, m_payloads.data(), m_payloads.size());
            }
            else
            {
            	uint32_t cur_r = 0, received_symbols = 0;

				m_encoder->nested()->set_num_nonzero_expansion(cur_r, expansion);


				for (uint8_t* payload : m_payloads)
                {
					//using like-systematic the extra coefficient, random the one when send extra packets
					if (cur_r < expansion)
					{
						if (symbols - threshold <= m_encoded_symbols)
						{
							cur_r = expansion;
							m_encoder->nested()->set_num_nonzero_expansion(cur_r, expansion, true);
							//std::cout<<"r = "<<cur_r<<"--> symbols sent:"<<sent_symbols<<std::endl;

						}
						else{
							//to avoid cur_r being never increased when symbols - threshold - expansion < 0
							int sub = symbols - threshold - expansion;
							if ((sub < 0) || (symbols - threshold - expansion <= m_encoded_symbols))
							{
								cur_r += 1;
								m_encoder->nested()->set_num_nonzero_expansion(cur_r, expansion, true);
//								std::cout<<"r = "<<cur_r<<"--> symbols sent:"<<sent_symbols<<std::endl;

							}
						}


					}
//					if (m_encoded_symbols >= symbols)
//						m_encoder->nested()->set_num_nonzero_expansion(cur_r, expansion, false, true);

                    ++m_encoded_symbols;

                    m_encoder->write_payload(payload);


                }
            }

            /// @todo Revert to the original input data by re-applying the
            /// prefix to the input data. This is needed since the
            /// benchmark loops and re-encodes the same data. If we did
            /// not re-apply the prefix the input data would be corrupted
            /// in the second iteration, causing decoding to produce
            /// incorrect output data. In a real-world application this
            /// would typically not be needed. For this reason the
            /// prime2325 results encoding results will show a lower
            /// throughput than what we can expect to see in an actual
            /// application. Currently we don't have a good solution for
            /// this. Possible solutions would be to copy the input data,
            /// however in that way we will also see a performance hit.
            if (fifi::is_prime2325<typename Encoder::field_type>::value)
            {
                // Apply the negated prefix
                fifi::apply_prefix(storage::storage(m_data_in), ~m_prefix);
            }
        }


    void decode_payloads()
    {
        // If the decoder uses shallow storage, we have to initialize
        // its decoding buffer
        if (kodo_core::has_set_mutable_symbols<Decoder>::value)
        {
            kodo_core::set_mutable_symbols(*m_decoder, storage::storage(m_data_out));
        }

        if (std::is_same<BlockCoding, block_coding_on>::value &&
            kodo_core::has_read_payloads<Decoder>::value)
        {
            kodo_core::read_payloads(
                *m_decoder, m_payloads.data(), m_payloads.size());

            m_processed_symbols += (uint32_t) m_payloads.size();
        }
        else
        {
            for (const uint8_t* payload : m_payloads)
            {
                /// @todo This copy would typically not be performed by an
                /// actual application, however since the benchmark
                /// performs several iterations while decoding the same data we
                /// have to copy the input data to avoid corrupting it.
                /// The decoder works on the data "in-place", therefore we
                /// would corrupt the payloads if we did not copy them.
                ///
                /// Changing this would most likely improve the throughput
                /// of the decoders. We are open to suggestions :)
               
//         	if(m_distribution(m_random_generator))
//            	{
////   	    			std::cout<<"Error took place" << std::endl;
//       	    			continue;
//  	    		}

         		std::copy_n(payload, m_decoder->payload_size(),
                            m_temp_payload.data());

                m_decoder->read_payload(m_temp_payload.data());

                ++m_processed_symbols;
                ++m_used;

                //std::cout<<"Decoder data:" << m_processed_symbols <<std::endl;

                if (m_decoder->is_complete())
                {
                	break;
                }
            }
        }

        if (m_decoder->is_complete())
        {
            // If the decoder uses deep storage, we have to copy
            // its decoding buffers for verification.
            // This would also be necessary in real applications, so it
            // should be part of the timed benchmark.
            if (kodo_core::has_deep_symbol_storage<Decoder>::value)
            {
                m_decoder->copy_from_symbols(storage::storage(m_data_out));
            }

            if (fifi::is_prime2325<typename Decoder::field_type>::value)
            {
                // Now we have to apply the negated prefix to the decoded data
                fifi::apply_prefix(storage::storage(m_data_out), ~m_prefix);
            }

            m_recovered_symbols += m_decoder->symbols();

            //std::cout<<"Decode completely:" <<m_recovered_symbols <<std::endl;
        }
    }

    /// Run the encoder
    void run_fulcrum_encode()
    {
  	  gauge::config_set cs = get_current_configuration();
  	  uint32_t symbols = cs.get_value<uint32_t>("symbols");

  	  uint32_t expansion = cs.get_value<uint32_t>("expansion");

  	  if (kodo_core::has_set_systematic_off<Encoder>::value)
  		  kodo_core::set_systematic_off(*m_encoder);

        // The clock is running
        RUN
        {
            // We have to make sure the encoder is in a "clean" state
            m_encoder->initialize(*m_encoder_factory);

            encode_payloads_fulcrum(symbols, expansion);
        }
    }
    void run_DTEP_encode()
      {
    	  gauge::config_set cs = get_current_configuration();
    	  uint32_t symbols = cs.get_value<uint32_t>("symbols");



    	  uint32_t expansion = cs.get_value<uint32_t>("expansion");
    	  if (kodo_core::has_set_systematic_off<Encoder>::value)
    		  kodo_core::set_systematic_off(*m_encoder);

          // The clock is running
          RUN
          {
              // We have to make sure the encoder is in a "clean" state
              m_encoder->initialize(*m_encoder_factory);

              encode_payloads_DTEP(symbols, expansion);
          }
      }

    void run_STEP_encode()
      {
    	  gauge::config_set cs = get_current_configuration();
    	  uint32_t symbols = cs.get_value<uint32_t>("symbols");
    	  uint32_t expansion = cs.get_value<uint32_t>("expansion");

    	  uint32_t threshold = cs.get_value<uint32_t>("threshold");

    	  if (kodo_core::has_set_systematic_off<Encoder>::value)
    		  kodo_core::set_systematic_off(*m_encoder);

          // The clock is running
          RUN
          {
              // We have to make sure the encoder is in a "clean" state
              m_encoder->initialize(*m_encoder_factory);

              encode_payloads_STEP(symbols, expansion, threshold);
          }
      }


    /// Run the decoder
    void run_fulcrum_decode()
    {
    	gauge::config_set cs = get_current_configuration();
		uint32_t symbols = cs.get_value<uint32_t>("symbols");
		uint32_t expansion = cs.get_value<uint32_t>("expansion");
		if (kodo_core::has_set_systematic_off<Encoder>::value)
		  kodo_core::set_systematic_off(*m_encoder);
        // Encode some data
        encode_payloads_fulcrum(symbols, expansion);

        // Zero the data buffer for the decoder
        std::fill_n(m_data_out.begin(), m_data_out.size(), 0);

        // The clock is running
        RUN
        {
            // We have to make sure the decoder is in a "clean" state
            // i.e. no symbols already decoded.
            m_decoder->initialize(*m_decoder_factory);

            // Decode the payloads
            decode_payloads();
        }
    }
    void run_DTEP_decode()
        {
    		gauge::config_set cs = get_current_configuration();
			uint32_t symbols = cs.get_value<uint32_t>("symbols");
			uint32_t expansion = cs.get_value<uint32_t>("expansion");
			if (kodo_core::has_set_systematic_off<Encoder>::value)
			  kodo_core::set_systematic_off(*m_encoder);
			// Encode some data
            encode_payloads_DTEP(symbols, expansion);

            // Zero the data buffer for the decoder
            std::fill_n(m_data_out.begin(), m_data_out.size(), 0);

            // The clock is running
            RUN
            {
                // We have to make sure the decoder is in a "clean" state
                // i.e. no symbols already decoded.
                m_decoder->initialize(*m_decoder_factory);

                // Decode the payloads
                decode_payloads();
            }
        }


    void run_STEP_decode()
           {
       		gauge::config_set cs = get_current_configuration();
   			uint32_t symbols = cs.get_value<uint32_t>("symbols");
   			uint32_t expansion = cs.get_value<uint32_t>("expansion");
   			uint32_t threshold = cs.get_value<uint32_t>("threshold");
   			if (kodo_core::has_set_systematic_off<Encoder>::value)
   			  kodo_core::set_systematic_off(*m_encoder);

   			// Encode some data
               encode_payloads_STEP(symbols, expansion, threshold);

               // Zero the data buffer for the decoder
               std::fill_n(m_data_out.begin(), m_data_out.size(), 0);

               // The clock is running
               RUN
               {
                   // We have to make sure the decoder is in a "clean" state
                   // i.e. no symbols already decoded.
                   m_decoder->initialize(*m_decoder_factory);

                   // Decode the payloads
                   decode_payloads();
               }
           }

    void run_body_fulcrum()
    {
        gauge::config_set cs = get_current_configuration();

        std::string type = cs.get_value<std::string>("type");

        if (type == "encoder")
        {
        	run_fulcrum_encode();
        }
        else if (type == "decoder")
        {
            run_fulcrum_decode();
        }
        else
        {
            assert(0 && "Unknown benchmark type");
        }
    }

    void run_body_dtep()
        {
            gauge::config_set cs = get_current_configuration();

            std::string type = cs.get_value<std::string>("type");

            if (type == "encoder")
            {
            	run_DTEP_encode();
            }
            else if (type == "decoder")
            {
                run_DTEP_decode();
            }
            else
            {
                assert(0 && "Unknown benchmark type");
            }
        }
    void run_body_step()
           {
               gauge::config_set cs = get_current_configuration();

               std::string type = cs.get_value<std::string>("type");

               if (type == "encoder")
               {
               	run_STEP_encode();
               }
               else if (type == "decoder")
               {
                   run_STEP_decode();
               }
               else
               {
                   assert(0 && "Unknown benchmark type");
               }
           }


protected:

    /// The decoder factory
    std::shared_ptr<decoder_factory> m_decoder_factory;

    /// The encoder factory
    std::shared_ptr<encoder_factory> m_encoder_factory;


    /// The encoder to use
    encoder_ptr m_encoder;

    /// The number of encoded symbols
    uint32_t m_encoded_symbols;


    /// The decoder to use
    decoder_ptr m_decoder;

    /// The number of symbols recovered by the decoder
    uint32_t m_recovered_symbols;

    /// The number of symbols processed by the decoder
    uint32_t m_processed_symbols;

    /// The input data
    std::vector<uint8_t> m_data_in;

    /// The output data
    std::vector<uint8_t> m_data_out;

    /// Buffer storing the encoded payloads before adding them to the
    /// decoder. This is necessary since the decoder will "work on"
    /// the encoded payloads directly. Therefore if we want to be able
    /// to run multiple iterations with the same encoded paylaods we
    /// have to copy them before injecting them into the decoder. This
    /// of course has a negative impact on the decoding throughput.
    std::vector<uint8_t> m_temp_payload;


    /// Contiguous buffer for coded payloads
    std::vector<uint8_t> m_payload_buffer;


    /// Pointers to each payload in the payload buffer
    std::vector<uint8_t*> m_payloads;

    /// Multiplication factor for payload_count
    uint32_t m_factor;

    /// Prefix used when testing with the prime2325 finite field
    uint32_t m_prefix;

    boost::random::mt19937 m_random_generator;

    boost::random::bernoulli_distribution<> m_distribution;

    uint32_t m_used = 0;

    uint32_t num_regions = 0;

    double m_time;
};
