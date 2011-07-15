/*  vim:expandtab:shiftwidth=2:tabstop=2:smarttab:
 * 
 *  Gearmand client and server library.
 *
 *  Copyright (C) 2011 Data Differential, http://datadifferential.com/
 *  Copyright (C) 2008 Brian Aker, Eric Day
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are
 *  met:
 *
 *      * Redistributions of source code must retain the above copyright
 *  notice, this list of conditions and the following disclaimer.
 *
 *      * Redistributions in binary form must reproduce the above
 *  copyright notice, this list of conditions and the following disclaimer
 *  in the documentation and/or other materials provided with the
 *  distribution.
 *
 *      * The names of its contributors may not be used to endorse or
 *  promote products derived from this software without specific prior
 *  written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */


/**
 * @file
 * @brief Blob slap worker utility
 */

#include <benchmark/benchmark.h>
#include <boost/program_options.hpp>
#include <cerrno>
#include <cstdio>
#include <climits>
#include <iostream>
#include <vector>
#include "util/daemon.h"

static void *worker_fn(gearman_job_st *job, void *context,
                       size_t *result_size, gearman_return_t *ret_ptr);

static gearman_return_t shutdown_fn(gearman_job_st*, void* /* context */)
{
  return GEARMAN_SHUTDOWN;
}


int main(int args, char *argv[])
{
  gearman_benchmark_st benchmark;
  bool opt_daemon;
  bool opt_chunk;
  bool opt_status;
  bool opt_unique;
  int32_t timeout;
  uint32_t count= UINT_MAX;
  in_port_t port;
  std::string host;
  std::vector<std::string>* functions= NULL;
  std::string verbose_string;
  boost::program_options::options_description desc("Options");
  desc.add_options()
    ("help", "Options related to the program.")
    ("host,h", boost::program_options::value<std::string>(&host)->default_value("localhost"),"Connect to the host")
    ("port,p", boost::program_options::value<in_port_t>(&port)->default_value(GEARMAN_DEFAULT_TCP_PORT), "Port number use for connection")
    ("count,c", boost::program_options::value<uint32_t>(&count)->default_value(0), "Number of jobs to run before exiting")
    ("timeout,u", boost::program_options::value<int32_t>(&timeout)->default_value(-1), "Timeout in milliseconds")
    ("chunk", boost::program_options::bool_switch(&opt_chunk)->default_value(false), "Send result back in data chunks")
    ("status,s", boost::program_options::bool_switch(&opt_status)->default_value(false), "Send status updates and sleep while running job")
    ("unique,u", boost::program_options::bool_switch(&opt_unique)->default_value(false), "When grabbing jobs, grab the uniqie id")
    ("daemon,d", boost::program_options::bool_switch(&opt_daemon)->default_value(false), "Daemonize")
    ("function,f", boost::program_options::value(functions), "Function to use.")
    ("verbose,v", boost::program_options::value(&verbose_string)->default_value("v"), "Increase verbosity level by one.")
            ;

  boost::program_options::variables_map vm;
  try
  {
    boost::program_options::store(boost::program_options::parse_command_line(args, argv, desc), vm);
    boost::program_options::notify(vm);
  }
  catch(std::exception &e)
  { 
    std::cout << e.what() << std::endl;
    return EXIT_FAILURE;
  }

  if (vm.count("help"))
  {
    std::cout << desc << std::endl;
    return EXIT_SUCCESS;
  }

  if (opt_daemon)
  {
    gearmand::daemonize(false, true);
  }

  if (opt_daemon)
  {
    gearmand::daemon_is_ready(benchmark.verbose == 0);
  }

  gearman_worker_st *worker;
  if (not (worker= gearman_worker_create(NULL)))
  {
    std::cerr << "Failed to allocate worker" << std::endl;
    exit(EXIT_FAILURE);
  }


  benchmark.verbose= static_cast<uint8_t>(verbose_string.length());

  if (gearman_failed(gearman_worker_add_server(worker, host.c_str(), port)))
  {
    std::cerr << "Failed while adding server " << host << ":" << port << " :" << gearman_worker_error(worker) << std::endl;
    exit(EXIT_FAILURE);
  }

  gearman_function_t shutdown_function= gearman_function_create(shutdown_fn);
  if (gearman_failed(gearman_worker_define_function(worker,
						    gearman_literal_param("shutdown"), 
						    shutdown_function,
						    0, 0)))
  {
    std::cerr << "Failed to add shutdown function: " << gearman_worker_error(worker) << std::endl;
    exit(EXIT_FAILURE);
  }

  if (functions and functions->size())
  {
    for (std::vector<std::string>::iterator iter= functions->begin();
         iter != functions->end();
         iter++)
    {
      if (gearman_failed(gearman_worker_add_function(worker,
                                                     (*iter).c_str(), 0,
                                                     worker_fn, &benchmark)))
      {
        std::cerr << "Failed to add default function: " << gearman_worker_error(worker) << std::endl;
        exit(EXIT_FAILURE);
      }
    }
  }
  else
  {
    if (gearman_failed(gearman_worker_add_function(worker,
                                                   GEARMAN_BENCHMARK_DEFAULT_FUNCTION, 0,
                                                   worker_fn, &benchmark)))
    {
      std::cerr << "Failed to add default function: " << gearman_worker_error(worker) << std::endl;
      exit(EXIT_FAILURE);
    }
  }

  gearman_worker_set_timeout(worker, timeout);

  do
  {
    gearman_return_t rc= gearman_worker_work(worker);

    if (rc == GEARMAN_SHUTDOWN)
    {
      if (benchmark.verbose > 0)
      {
        std::cerr << "shutdown" << std::endl;
      }
      break;
    }
    else if (gearman_failed(rc))
    {
      std::cerr << "gearman_worker_work(): " << gearman_worker_error(worker) << std::endl;
      break;
    }

    count--;
  } while(count);

  gearman_worker_free(worker);

  return 0;
}

static void *worker_fn(gearman_job_st *job, void *context,
                       size_t *, gearman_return_t *ret_ptr)
{
  gearman_benchmark_st *benchmark= static_cast<gearman_benchmark_st *>(context);

  if (benchmark->verbose > 0)
    benchmark_check_time(benchmark);

  if (benchmark->verbose > 1)
  {
    std::cout << "Job=%s (" << gearman_job_workload_size(job) << ")" << std::endl;
  }

  *ret_ptr= GEARMAN_SUCCESS;
  return NULL;
}
