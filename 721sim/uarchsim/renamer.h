#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

class renamer {
private:
    /////////////////////////////////////////////////////////////////////
    // Put private class variables here.
    /////////////////////////////////////////////////////////////////////

    /////////////////////////////////////////////////////////////////////
    // Structure 1: Rename Map Table
    // Entry contains: physical register mapping
    /////////////////////////////////////////////////////////////////////
    uint64_t *rmt; 

    /////////////////////////////////////////////////////////////////////
    // Structure 2: Architectural Map Table
    // Entry contains: physical register mapping
    /////////////////////////////////////////////////////////////////////


    uint64_t map_table_size; //AMT and RMT size
    /////////////////////////////////////////////////////////////////////
    // Structure 3: Free List
    //
    // Entry contains: physical register number
    //
    // Notes:
    // * Structure includes head, tail, and their phase bits.
    //
    // Usage:
    //  - free entry is at the head, move head pointer at rename
    //  - add newly freed entry at the tail, move tail pointer at retire
    /////////////////////////////////////////////////////////////////////
    typedef struct free_list_t{
        uint64_t head, head_phase;
        uint64_t tail, tail_phase;
        uint64_t *list;
    } free_list;

    uint64_t free_list_size;    
    free_list fl; 

    /////////////////////////////////////////////////////////////////////
    // Structure 4: Active List
    //
    // Entry contains:
    //
    // ----- Fields related to destination register.
    // 1. destination flag (indicates whether or not the instr. has a
    //    destination register)
    // 2. logical register number of the instruction's destination
    // 3. physical register number of the instruction's destination
    // ----- Fields related to completion status.
    // 4. completed bit
    // ----- Fields for signaling offending instructions.
    // 5. exception bit
    // 6. load violation bit
    //    * Younger load issued before an older conflicting store.
    //      This can happen when speculative memory disambiguation
    //      is enabled.
    // 7. branch misprediction bit
    //    * At present, not ever set by the pipeline. It is simply
    //      available for deferred-recovery Approaches #1 or #2.
    //      Project 1 uses Approach #5, however.
    // 8. value misprediction bit
    //    * At present, not ever set by the pipeline. It is simply
    //      available for deferred-recovery Approaches #1 or #2,
    //      if value prediction is added (e.g., research projects).
    // ----- Fields indicating special instruction types.
    // 9. load flag (indicates whether or not the instr. is a load)
    // 10. store flag (indicates whether or not the instr. is a store)
    // 11. branch flag (indicates whether or not the instr. is a branch)
    // 12. amo flag (whether or not instr. is an atomic memory operation)
    // 13. csr flag (whether or not instr. is a system instruction)
    // ----- Other fields.
    // 14. program counter of the instruction
    //
    // Notes:
    // * Structure includes head, tail, and their phase bits.
    /////////////////////////////////////////////////////////////////////


    /////////////////////////////////////////////////////////////////////
    // Structure 5: Physical Register File
    // Entry contains: value
    //
    // Notes:
    // * The value must be of the following type: uint64_t
    //   (#include <inttypes.h>, already at top of this file)
    /////////////////////////////////////////////////////////////////////
    uint64_t *prf;

    /////////////////////////////////////////////////////////////////////
    // Structure 6: Physical Register File Ready Bit Array
    // Entry contains: ready bit
    /////////////////////////////////////////////////////////////////////
    uint64_t *prf_ready;
    uint64_t *prf_usage_counter;
    uint64_t *prf_unmapped;

    uint64_t num_phys_reg;

    typedef struct chkpt_t{
        uint64_t *rmt;
        bool     amo;
        bool     csr;
        bool     exception;
        uint64_t load_counter;
        uint64_t store_counter;
        uint64_t branch_counter;
        uint64_t uncompleted_instruction_counter;
        uint64_t *unmapped_bits;
    }chkpt;

    uint64_t num_checkpoints;
    chkpt *checkpoint_buffer;
    uint64_t chkpt_buffer_head;
    uint64_t chkpt_buffer_tail;
    uint64_t chkpt_buffer_head_phase;
    uint64_t chkpt_buffer_tail_phase;


    /////////////////////////////////////////////////////////////////////
    // Private functions.
    // e.g., a generic function to copy state from one map to another.
    /////////////////////////////////////////////////////////////////////
    //////////////////////////////////////////
    // Helper functions later implemented  .//
    //////////////////////////////////////////

    bool free_list_is_empty();
    bool free_list_is_full();
    bool push_free_list(uint64_t phys_reg);
    uint64_t pop_free_list();
    int free_list_regs_available();
    void restore_free_list();
    uint64_t get_free_checkpoint_count();
    bool checkpoint_buffer_is_empty();
    bool checkpoint_buffer_is_full();

    /*
    void print_free_list();
    void print_amt();
    void print_rmt();
    void print_prf();
    void print_prf_ready();
    void print_active_list(bool between_head_and_tail);
    */
    bool reg_in_rmt(uint64_t);
    void assert_free_list_invariance(); 

public:
    ////////////////////////////////////////
    // Public functions.
    ////////////////////////////////////////

    /////////////////////////////////////////////////////////////////////
    // This is the constructor function.
    // When a renamer object is instantiated, the caller indicates:
        // 1. The number of logical registers (e.g., 32).
    // 2. The number of physical registers (e.g., 128).
    // 3. The maximum number of unresolved branches.
    //    Requirement: 1 <= n_branches <= 64.
    // 4. The maximum number of active instructions (Active List size).
    //
    // Tips:
    //
    // Assert the number of physical registers > number logical registers.
    // Assert 1 <= n_branches <= 64.
    // Assert n_active > 0.
    // Then, allocate space for the primary data structures.
    // Then, initialize the data structures based on the knowledge
    // that the pipeline is intially empty (no in-flight instructions yet).
    /////////////////////////////////////////////////////////////////////
    renamer(uint64_t n_log_regs,
        uint64_t n_phys_regs,
        uint64_t n_chkpts,
        uint64_t n_active);

    /////////////////////////////////////////////////////////////////////
    // This is the destructor, used to clean up memory space and
    // other things when simulation is done.
    // I typically don't use a destructor; you have the option to keep
    // this function empty.
    /////////////////////////////////////////////////////////////////////
    ~renamer();


    //////////////////////////////////////////
    // Functions related to Rename Stage.   //
    //////////////////////////////////////////

    /////////////////////////////////////////////////////////////////////
    // The Rename Stage must stall if there aren't enough free physical
    // registers available for renaming all logical destination registers
    // in the current rename bundle.
    //
    // Inputs:
    // 1. bundle_dst: number of logical destination registers in
    //    current rename bundle
    //
    // Return value:
    // Return "true" (stall) if there aren't enough free physical
    // registers to allocate to all of the logical destination registers
    // in the current rename bundle.
    /////////////////////////////////////////////////////////////////////
    bool stall_reg(uint64_t bundle_dst);


    /////////////////////////////////////////////////////////////////////
    // This function is used to rename a single source register.
    //
    // Inputs:
    // 1. log_reg: the logical register to rename
    //
    // Return value: physical register name
    /////////////////////////////////////////////////////////////////////
    uint64_t rename_rsrc(uint64_t log_reg);
    void inc_usage_counter(uint64_t phys_reg);
    void dec_usage_counter(uint64_t phys_reg);

    /////////////////////////////////////////////////////////////////////
    // This function is used to rename a single destination register.
    //
    // Inputs:
    // 1. log_reg: the logical register to rename
    //
    // Return value: physical register name
    /////////////////////////////////////////////////////////////////////
    uint64_t rename_rdst(uint64_t log_reg);

    /////////////////////////////////////////////////////////////////////
    // This function creates a new branch checkpoint.
    //
    // Inputs: none.
    //
    // Output:
    // 1. The function returns the branch's ID. When the branch resolves,
    //    its ID is passed back to the renamer via "resolve()" below.
    //
    // Tips:
    //
    // Allocating resources for the branch (a GBM bit and a checkpoint):
    // * Find a free bit -- i.e., a '0' bit -- in the GBM. Assert that
    //   a free bit exists: it is the user's responsibility to avoid
    //   a structural hazard by calling stall_branch() in advance.
    // * Set the bit to '1' since it is now in use by the new branch.
    // * The position of this bit in the GBM is the branch's ID.
    // * Use the branch checkpoint that corresponds to this bit.
    // 
    // The branch checkpoint should contain the following:
    // 1. Shadow Map Table (checkpointed Rename Map Table)
    // 2. checkpointed Free List head pointer and its phase bit
    // 3. checkpointed GBM
    /////////////////////////////////////////////////////////////////////
    void checkpoint();


    //whether or not to stall rename due to insufficent checkpoints
    bool stall_checkpoint(uint64_t bundle_chkpt);

    //get checkpoint ID that's each instruction is associated with
    //it is the nearest prior checkpoint
    uint64_t get_checkpoint_ID(bool load, bool store, bool branch, bool amo, bool csr);

    //set exception
    void set_exception(uint64_t checkpoint_ID);

    //////////////////////////////////////////
    // Functions related to Dispatch Stage. //
    //////////////////////////////////////////


    /////////////////////////////////////////////////////////////////////
    // Test the ready bit of the indicated physical register.
    // Returns 'true' if ready.
    /////////////////////////////////////////////////////////////////////
    bool is_ready(uint64_t phys_reg);

    /////////////////////////////////////////////////////////////////////
    // Clear the ready bit of the indicated physical register.
    /////////////////////////////////////////////////////////////////////
    void clear_ready(uint64_t phys_reg);


    //////////////////////////////////////////
    // Functions related to the Reg. Read   //
    // and Execute Stages.                  //
    //////////////////////////////////////////

    /////////////////////////////////////////////////////////////////////
    // Return the contents (value) of the indicated physical register.
    /////////////////////////////////////////////////////////////////////
    uint64_t read(uint64_t phys_reg);

    /////////////////////////////////////////////////////////////////////
    // Set the ready bit of the indicated physical register.
    /////////////////////////////////////////////////////////////////////
    void set_ready(uint64_t phys_reg);


    //////////////////////////////////////////
    // Functions related to Writeback Stage.//
    //////////////////////////////////////////

    /////////////////////////////////////////////////////////////////////
    // Write a value into the indicated physical register.
    /////////////////////////////////////////////////////////////////////
    void write(uint64_t phys_reg, uint64_t value);

    /////////////////////////////////////////////////////////////////////
    // Set the completed bit of the indicated entry in the Active List.
    /////////////////////////////////////////////////////////////////////
    void set_complete(uint64_t checkpoint_ID);

    /////////////////////////////////////////////////////////////////////
    // This function is for handling branch resolution.
    //
    // Inputs:
    // 1. AL_index: Index of the branch in the Active List.
    // 2. branch_ID: This uniquely identifies the branch and the
    //    checkpoint in question.  It was originally provided
    //    by the checkpoint function.
    // 3. correct: 'true' indicates the branch was correctly
    //    predicted, 'false' indicates it was mispredicted
    //    and recovery is required.
    //
    // Outputs: none.
    //
    // Tips:
    //
    // While recovery is not needed in the case of a correct branch,
    // some actions are still required with respect to the GBM and
    // all checkpointed GBMs:
    // * Remember to clear the branch's bit in the GBM.
    // * Remember to clear the branch's bit in all checkpointed GBMs.
    //
    // In the case of a misprediction:
    // * Restore the GBM from the branch's checkpoint. Also make sure the
    //   mispredicted branch's bit is cleared in the restored GBM,
    //   since it is now resolved and its bit and checkpoint are freed.
    // * You don't have to worry about explicitly freeing the GBM bits
    //   and checkpoints of branches that are after the mispredicted
    //   branch in program order. The mere act of restoring the GBM
    //   from the checkpoint achieves this feat.
    // * Restore the RMT using the branch's checkpoint.
    // * Restore the Free List head pointer and its phase bit,
    //   using the branch's checkpoint.
    // * Restore the Active List tail pointer and its phase bit
    //   corresponding to the entry after the branch's entry.
    //   Hints:
    //   You can infer the restored tail pointer from the branch's
    //   AL_index. You can infer the restored phase bit, using
    //   the phase bit of the Active List head pointer, where
    //   the restored Active List tail pointer is with respect to
    //   the Active List head pointer, and the knowledge that the
    //   Active List can't be empty at this moment (because the
    //   mispredicted branch is still in the Active List).
    // * Do NOT set the branch misprediction bit in the Active List.
    //   (Doing so would cause a second, full squash when the branch
    //   reaches the head of the Active List. We don’t want or need
    //   that because we immediately recover within this function.)
    /////////////////////////////////////////////////////////////////////
    void resolve(uint64_t AL_index,
             uint64_t branch_ID,
             bool correct);

    //////////////////////////////////////////
    // Functions related to Retire Stage.   //
    //////////////////////////////////////////

    ///////////////////////////////////////////////////////////////////
    // This function allows the caller to examine the instruction at the head
    // of the Active List.
    //
    // Input arguments: none.
    //
    // Return value:
    // * Return "true" if the Active List is NOT empty, i.e., there
    //   is an instruction at the head of the Active List.
    // * Return "false" if the Active List is empty, i.e., there is
    //   no instruction at the head of the Active List.
    //
    // Output arguments:
    // Simply return the following contents of the head entry of
    // the Active List.  These are don't-cares if the Active List
    // is empty (you may either return the contents of the head
    // entry anyway, or not set these at all).
    // * completed bit
    // * exception bit
    // * load violation bit
    // * branch misprediction bit
    // * value misprediction bit
    // * load flag (indicates whether or not the instr. is a load)
    // * store flag (indicates whether or not the instr. is a store)
    // * branch flag (indicates whether or not the instr. is a branch)
    // * amo flag (whether or not instr. is an atomic memory operation)
    // * csr flag (whether or not instr. is a system instruction)
    // * program counter of the instruction
    /////////////////////////////////////////////////////////////////////

    bool precommit(uint64_t &chkpt_id,
                        uint64_t &num_loads,
                        uint64_t &num_stores,
                        uint64_t &num_branches,
                        bool &amo, bool &csr, bool &exception);
    /////////////////////////////////////////////////////////////////////
    // This function commits the instruction at the head of the Active List.
    //
    // Tip (optional but helps catch bugs):
    // Before committing the head instruction, assert that it is valid to
    // do so (use assert() from standard library). Specifically, assert
    // that all of the following are true:
    // - there is a head instruction (the active list isn't empty)
    // - the head instruction is completed
    // - the head instruction is not marked as an exception
    // - the head instruction is not marked as a load violation
    // It is the caller's (pipeline's) duty to ensure that it is valid
    // to commit the head instruction BEFORE calling this function
    // (by examining the flags returned by "precommit()" above).
    // This is why you should assert() that it is valid to commit the
    // head instruction and otherwise cause the simulator to exit.
    /////////////////////////////////////////////////////////////////////
    void commit(uint64_t log_reg);

    //////////////////////////////////////////////////////////////////////
    // Squash the renamer class.
    //
    // Squash all instructions in the Active List and think about which
    // sructures in your renamer class need to be restored, and how.
    //
    // After this function is called, the renamer should be rolled-back
    // to the committed state of the machine and all renamer state
    // should be consistent with an empty pipeline.
    /////////////////////////////////////////////////////////////////////
    void squash();

};
