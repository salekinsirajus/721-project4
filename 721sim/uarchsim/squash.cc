#include "pipeline.h"

void pipeline_t::dec_for_pipeline_registers(uint64_t index){
    printf("START decrementing Source and Destination registers\n", PAY.buf[index].checkpoint_ID, PAY.buf[index].pc);
    if (PAY.buf[index].A_valid){
        printf("%X: decrementing usage counter for A_phys_reg %d\n", PAY.buf[index].pc,
            PAY.buf[index].A_phys_reg
        );
        REN->dec_usage_counter(PAY.buf[index].A_phys_reg);
    }
    if (PAY.buf[index].B_valid){
        printf("%X: decrementing usage counter for B_phys_reg %d\n", PAY.buf[index].pc,
            PAY.buf[index].B_phys_reg
        );
        REN->dec_usage_counter(PAY.buf[index].B_phys_reg);
    }
    if (PAY.buf[index].D_valid){
        printf("%X: decrementing usage counter for D_phys_reg %d\n", PAY.buf[index].pc,
            PAY.buf[index].D_phys_reg
        );
        REN->dec_usage_counter(PAY.buf[index].D_phys_reg);
    }
    if (PAY.buf[index].C_valid){
        printf("%X: decrementing usage counter for C_phys_reg %d\n", PAY.buf[index].pc,
            PAY.buf[index].C_phys_reg
        );
        REN->dec_usage_counter(PAY.buf[index].C_phys_reg);
    }

    printf("DONE Decremenging Source and Dest registers\n", PAY.buf[index].checkpoint_ID, PAY.buf[index].pc);
}

void pipeline_t::squash_complete(reg_t jump_PC) {
	unsigned int i, j;

	//////////////////////////
	// Fetch Stage
	//////////////////////////
  
	FetchUnit->flush(jump_PC);

	//////////////////////////
	// Decode Stage
	//////////////////////////

	for (i = 0; i < fetch_width; i++) {
		DECODE[i].valid = false;
	}

	//////////////////////////
	// Rename1 Stage
	//////////////////////////

	FQ.flush();

	//////////////////////////
	// Rename2 Stage
	//////////////////////////

	for (i = 0; i < dispatch_width; i++) {
		RENAME2[i].valid = false;
	}

        //
        // FIX_ME #17c
        // Squash the renamer.
        //

        // FIX_ME #17c BEGIN
        REN->squash();
        // FIX_ME #17c END


	//////////////////////////
	// Dispatch Stage
	//////////////////////////

	for (i = 0; i < dispatch_width; i++) {
		if (DISPATCH[i].valid){
            dec_for_pipeline_registers(DISPATCH[i].index);
        }
		DISPATCH[i].valid = false;
	}

	//////////////////////////
	// Schedule Stage
	//////////////////////////

	IQ.flush();

	//////////////////////////
	// Register Read Stage
	// Execute Stage
	// Writeback Stage
	//////////////////////////

    uint64_t idx;
	for (i = 0; i < issue_width; i++) {
		if (Execution_Lanes[i].rr.valid){
            dec_for_pipeline_registers(Execution_Lanes[i].rr.index);
        }
		Execution_Lanes[i].rr.valid = false;
		for (j = 0; j < Execution_Lanes[i].ex_depth; j++){
           if(Execution_Lanes[i].ex[j].valid){
                dec_for_pipeline_registers(Execution_Lanes[i].ex[j].index);
            }
		   Execution_Lanes[i].ex[j].valid = false;
        }
		if (Execution_Lanes[i].wb.valid){
            dec_for_pipeline_registers(Execution_Lanes[i].wb.index);
        }
		Execution_Lanes[i].wb.valid = false;
	}

	LSU.flush();
}


//void pipeline_t::resolve(unsigned int branch_ID, bool correct) {
void pipeline_t::selective_squash(uint64_t squash_mask) {
    printf("in pipeline_t:selective_squash: passed squash mask: %lu\n", squash_mask);
	unsigned int i, j;

    // Squash all instructions in the Decode through Dispatch Stages.

    // Decode Stage:
    for (i = 0; i < fetch_width; i++) {
        DECODE[i].valid = false;
    }

    // Rename1 Stage:
    FQ.flush();

    // Rename2 Stage:
    for (i = 0; i < dispatch_width; i++) {
        RENAME2[i].valid = false;
    }

    // Dispatch Stage:
    printf("pipeline_t:selective_squash() START going over the instructions in dispatch pipeline regs\n");
    for (i = 0; i < dispatch_width; i++) {
        if ((DISPATCH[i].valid) && ((squash_mask & (1UL << PAY.buf[DISPATCH[i].index].checkpoint_ID)) != 0)){
            dec_for_pipeline_registers(DISPATCH[i].index);
        }
        DISPATCH[i].valid = false;
    }
    printf("pipeline_t:selective_squash() END going over the instructions in dispatch pipeline regs\n");

    // Selectively squash instructions after the branch, in the Schedule through Writeback Stages.

    // Schedule Stage:
    printf("pipeline_t:selective_squash: START calling IQ squash with squash mask\n");
    IQ.squash(squash_mask);
    printf("pipeline_t:selective_squash: END   calling IQ squash with squash mask\n");

    //WIP
    for (i = 0; i < issue_width; i++) {
        // Register Read Stage:
        if (Execution_Lanes[i].rr.valid && BIT_IS_ONE(squash_mask, PAY.buf[Execution_Lanes[i].rr.index].checkpoint_ID)) {
            //dec_for_pipeline_registers(Execution_Lanes[i].rr.index);
            if (PAY.buf[Execution_Lanes[i].rr.index].C_valid){
                printf("RR: squashing the dest register for %d\n", PAY.buf[Execution_Lanes[i].rr.index].C_phys_reg);
                REN->dec_usage_counter(PAY.buf[Execution_Lanes[i].rr.index].C_phys_reg);
            }
            Execution_Lanes[i].rr.valid = false;
        }

        // Execute Stage:
        for (j = 0; j < Execution_Lanes[i].ex_depth; j++) {
           if (Execution_Lanes[i].ex[j].valid && BIT_IS_ONE(squash_mask, PAY.buf[Execution_Lanes[i].ex[j].index].checkpoint_ID)) {
            //dec_for_pipeline_registers(Execution_Lanes[i].ex[j].index);
            if ((PAY.buf[Execution_Lanes[i].ex[j].index].C_valid) && ( j < Execution_Lanes[i].ex_depth - 1)){
                printf("EX: squashing the dest register for %d\n", PAY.buf[Execution_Lanes[i].ex[j].index].C_phys_reg);
                REN->dec_usage_counter(PAY.buf[Execution_Lanes[i].ex[j].index].C_phys_reg);
            }
            Execution_Lanes[i].ex[j].valid = false;
            }
        }

        // Writeback Stage:
        if (Execution_Lanes[i].wb.valid) { // && BIT_IS_ONE(squash_mask, PAY.buf[Execution_Lanes[i].wb.index].checkpoint_ID)) {
            //dec_for_pipeline_registers(Execution_Lanes[i].wb.index);
            Execution_Lanes[i].wb.valid = false;
        }
    }
}
