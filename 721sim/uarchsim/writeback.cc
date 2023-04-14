#include "pipeline.h"


void pipeline_t::writeback(unsigned int lane_number) {
   unsigned int index;

   // Check if there is an instruction in the Writeback Stage of the specified Execution Lane.
   if (Execution_Lanes[lane_number].wb.valid) {

      //////////////////////////////////////////////////////////////////////////////////////////////////////////
      // Get the instruction's index into PAY.
      //////////////////////////////////////////////////////////////////////////////////////////////////////////
      index = Execution_Lanes[lane_number].wb.index;

      //////////////////////////////////////////////////////////////////////////////////////////////////////////
      // FIX_ME #15
      // Resolve branches.
      //
      // Background: Here we are resolving a branch that previously made a checkpoint.
      //
      // If the branch was correctly predicted, then resolution consists of two steps:
      // * Tell the renamer module that the branch resolved correctly, so that it frees its corresponding checkpoint.
      // * Clear the branch's bit in the branch masks of instructions in the pipeline. Specifically, this
      //   involves instructions from the Rename Stage (where branch masks are first assigned) to the
      //   Writeback Stage (instructions leave the pipeline after Writeback, although they still hold
      //   entries in the Active List and Load/Store Queues).
      //
      // If the branch was mispredicted, then resolution consists of two high-level steps:
      // * Recover key units: the Fetch Unit, the renamer module (RMT, FL, AL), and the LSU.
      // * Squash instructions in the pipline that are logically after the branch -- those instructions
      //   that have the branch's bit in their branch masks -- meanwhile clearing that bit. Also, all
      //   instructions in the frontend stages are automatically squashed since they are by definition
      //   logically after the branch.
      //////////////////////////////////////////////////////////////////////////////////////////////////////////

     if (PAY.buf[index].next_pc != PAY.buf[index].c_next_pc) {
        // Branch was mispredicted.

        // Roll-back the Fetch Unit.
        FetchUnit->mispredict(PAY.buf[index].pred_tag,
                  (PAY.buf[index].c_next_pc != INCREMENT_PC(PAY.buf[index].pc)),
              PAY.buf[index].c_next_pc);

        // FIX_ME #15c
        // The simulator is running in real branch prediction mode, and the branch was mispredicted.
        // Recall the two high-level steps that are necessary in this case:
        // * Recover key units: the Fetch Unit, the renamer module (RMT, FL, AL), and the LSU.
        //   The Fetch Unit is recovered in the statements immediately preceding this comment (please study them for knowledge).
        //   The LSU is recovered in the statements immediately following this comment (please study them for knowledge).
        //   Your job for #15c will be to recover the renamer module, in between.
        // * Squash instructions in the pipeline that are logically after the branch. You will do this too, in #15d below.
        //
        // Restore the RMT, FL, and AL.
        //
        // Tips:
        // 1. See #15a, item 1.
        // 2. See #15a, item 2 -- EXCEPT in this case the branch was mispredicted, so specify not-correct instead of correct.
        //    This will restore the RMT, FL, and AL, and also free this and future checkpoints... etc.

        // FIX_ME #15c BEGIN
        //REN->resolve(PAY.buf[index].AL_index, PAY.buf[index].branch_ID, false);
        //WIP:
        uint64_t chkpt_id = PAY.buf[index].checkpoint_ID;
        uint64_t total_loads, total_stores, total_branches;
        //using the next checkpoint for branch misprediction
        uint64_t squash_mask = REN->rollback(chkpt_id, true, total_loads, total_stores, total_branches);
        // FIX_ME #15c END

        // Restore the LQ/SQ.
        LSU.restore(PAY.buf[index].LQ_index, PAY.buf[index].LQ_phase, PAY.buf[index].SQ_index, PAY.buf[index].SQ_phase);

        // FIX_ME #15d
        // Squash instructions after the branch in program order, in all pipeline registers and the IQ.
        //
        // Tips:
        // 1. At this point of the code, 'index' is the instruction's index into PAY.buf[] (payload).
        // 2. Squash instructions after the branch in program order.
        //    To do this, call the resolve() function with the appropriate arguments. This function does the work for you.
        //    * resolve() is a private function of the pipeline_t class, therefore, just call it literally as 'resolve'.
        //    * resolve() takes two arguments. The first argument is the branch's ID. The second argument is a flag that
        //      indicates whether or not the branch was predicted correctly: in this case it is not-correct.
        //    * See pipeline.h for details about the two arguments of resolve().

        // FIX_ME #15d BEGIN
        selective_squash(squash_mask);
        // FIX_ME #15d END

        // Rollback PAY to the point of the branch.
        PAY.rollback(index);
     }

      //////////////////////////////////////////////////////////////////////////////////////////////////////////
      // FIX_ME #16
      // Set completed bit in Active List.
      //
      // Tips:
      // 1. At this point of the code, 'index' is the instruction's index into PAY.buf[] (payload).
      // 2. Set the completed bit for this instruction in the Active List.
      //////////////////////////////////////////////////////////////////////////////////////////////////////////

      // FIX_ME #16 BEGIN
      REN->set_complete(PAY.buf[index].checkpoint_ID);
      // FIX_ME #16 END

      //////////////////////////////////////////////////////////////////////////////////////////////////////////
      // Remove the instruction from the Execution Lane.
      //////////////////////////////////////////////////////////////////////////////////////////////////////////
      Execution_Lanes[lane_number].wb.valid = false;
   }
}
