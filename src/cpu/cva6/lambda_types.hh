/**
 * lambda_types.hh
 *
 *  Author:   Pierre Ravenel
 * Created:   15/09/2024
 **/

#pragma once

#include <stdint.h>

#include <cstring>

#include "cpu/cva6/registerfile.hh"

namespace gem5 {
namespace cva6 {

/**
 * Main lambda container
*/
struct lambdakto1_t
{
  uint64_t pc_start;
  uint64_t pc_end;
  uint64_t pc_end_next;
  uint64_t size;
  uint64_t rd;
  uint64_t rd_val;

  bool operator==(const struct lambdakto1_t& o) const {
    return memcmp(this, &o, sizeof(lambdakto1_t)) == 0;
  }

  bool operator<(const struct lambdakto1_t& o) const {
    return memcmp(this, &o, sizeof(lambdakto1_t)) < 0;
  }

  std::string str() const {
    std::ostringstream os;

    // Lambda address
    os << std::hex;
    os <<  "\033[32m" << "@" << pc_start << "->" <<  pc_end;
    os << '<' << size << '>';
    // Lambda input
    os << '(' << "\033[39m";
    os << '*';
    os << "\033[32m" << ')' << "\033[39m";
    os << "\033[32m" << " |-> " << "\033[39m";
    // Lambda output
    os << '{' << riscvRegisterName(i2id(rd))
       << ": " << rd_val << '}';
    return os.str();
  }
};

struct lambda_inst_metadata_t
{
  /* Fetch */
  bool is_predicted = false;
  bool is_predicted_first = false;
  bool is_predicted_last = false;
  lambdakto1_t lambda;

  /* Pre Commit */
  bool is_const = false;

  /* Post Commit */
  /* Valid only if is_predicted_last */
  uint64_t check_pc_next = 0;
  uint64_t check_val = 0;
  uint64_t check_cpt_indempotance = 0;
  bool is_check_pc;
  bool is_check_val;
  bool is_check_indempotance;
  bool is_check_regalloc; // TODO ?

  bool do_check_pc_next(uint64_t pc_end_next){
    check_pc_next = pc_end_next;
    is_check_pc = check_pc_next == lambda.pc_end_next;
    return is_check_pc;
  }

  bool do_final_check(uint64_t val, uint64_t cpt, bool isk1){
    check_val = val;
    check_cpt_indempotance = cpt;
    is_check_val = lambda.rd_val == check_val;
    is_check_indempotance = check_cpt_indempotance >= lambda.size;
    is_check_regalloc = isk1;
    return is_check_pc &&
           is_check_val &&
           is_check_indempotance &&
           is_check_regalloc;
  }

  /*** Learning metadata ***/
  /* Learning step 1 */
  uint64_t is_in_trace_region = 0;
  /* Learning step 2 */
  bool is_learned = false;
  bool is_learned_first = false;
  bool is_learned_last = false;
  lambdakto1_t lambdalearn;
};



} // namespace cva6
} // namespace gem5
