#define __STDC_FORMAT_MACROS

#include "renamer.h"
#include <cassert>

renamer::renamer(uint64_t n_log_regs,
        uint64_t n_phys_regs,
        uint64_t n_branches,
        uint64_t n_active){
    
    //Run the assertions
    assert(n_phys_regs > n_log_regs);
    assert((1 <= n_branches) && (n_branches <= 64));
    assert(n_active > 0);
  
    //initialize the data structures
    map_table_size = n_log_regs;
    num_phys_reg = n_phys_regs;
    rmt = new uint64_t[n_log_regs]; 
    amt = new uint64_t[n_log_regs];
    prf = new uint64_t[n_phys_regs];
    prf_ready = new uint64_t[n_phys_regs];
    shadow_map_table_size = n_log_regs;

    uint64_t j;
    //setting the ready bits to 1 (meaning no pending registers)
    for (j=0; j < n_phys_regs; j++){
        prf_ready[j] = 1;
    }

    //AMT and RMT should have the same value at the beginning
    //However, not sure what would the content be. if amt[0] = 0, amt[1] = 0
    //then all the logical registers are mapped to p0. OTOH, amt[0] = 0 and
    //amt[1] = 1,..., amt[n] = n indicate r0->p0, r1->p1,..
    //the contents of the prf does not matter I suppose.
    for (j=0; j < n_log_regs; j++){
        amt[n_log_regs - 1 - j] = j;
        rmt[n_log_regs - 1 - j] = j;
    }

    //active list
    active_list_size = n_active;
    al.list = new al_entry[n_active]; 
    al.head = 0;
    al.tail = 0;
    al.head_phase = 0;
    al.tail_phase = 0;    
    assert(active_list_is_empty());

    for (j=0; j<n_active; j++){
        init_al_entry(&al.list[j]);
    }
    
    //free list; free_list_size = prf - n_log_regs (721ss-prf-2 slide, p19)
    free_list_size = n_phys_regs - n_log_regs;
    fl.list = new uint64_t[free_list_size];
    fl.head = 0;
    fl.tail = 0;
    fl.head_phase = 0;
    fl.tail_phase = 1;
    assert(this->free_list_is_full()); //free list should be full at init

    //Free list contains registers that are not allocated or committed
    //i.e. registers that are not in AMT or RMT.
    uint64_t i;
    for (i=0; i <free_list_size; i++){
        fl.list[i] = n_log_regs + i;
    }

    //checkpoint stuff
    GBM = 0;
    num_checkpoints = n_branches;
    checkpoints = new checkpoint_t[num_checkpoints];
}

bool renamer::stall_reg(uint64_t bundle_dst){
    uint64_t available_physical_regs = this->free_list_regs_available();
    if (available_physical_regs == UINT64_MAX) {
        printf("FATAL ERROR: free list is in incosistent state\n");
        exit(EXIT_FAILURE);
    }

    if (available_physical_regs < bundle_dst){
        return true;
    }

    return false;
}


uint64_t renamer::rename_rsrc(uint64_t log_reg){
    return this->rmt[log_reg]; 
}

bool renamer::free_list_is_empty(){
    if ((this->fl.head == this->fl.tail) && 
        (this->fl.head_phase == this->fl.tail_phase)) return true;

    return false;
}

bool renamer::free_list_is_full(){
    if ((this->fl.head == this->fl.tail) && 
        (this->fl.head_phase != this->fl.tail_phase)) return true;

    return false;
}

void renamer::restore_free_list(){
    this->fl.head = this->fl.tail;
    this->fl.head_phase = !this->fl.tail_phase;
}

bool renamer::push_free_list(uint64_t phys_reg){
    //if it's full, you cannot push more into it
    if (this->free_list_is_full()){
        return false;
    }

    this->fl.list[this->fl.tail] = phys_reg;
    this->fl.tail++;
    if (this->fl.tail == this->free_list_size){
        this->fl.tail = 0;
        this->fl.tail_phase = !this->fl.tail_phase;
    }

    assert_free_list_invariance();
    return true; 
}

uint64_t renamer::pop_free_list(){
    if (this->free_list_is_empty()){
        return UINT64_MAX;
    }

    uint64_t result;
    result = this->fl.list[this->fl.head];

    //advance the head pointer of the free list
    this->fl.head++;
    if (this->fl.head == this->free_list_size){
        //wrap around
        this->fl.head = 0;
        this->fl.head_phase = !this->fl.head_phase;
    }

    assert_free_list_invariance();
    return result;
}

void renamer::assert_free_list_invariance(){
    if (this->fl.head_phase != this->fl.tail_phase){
        assert(!(this->fl.head < this->fl.tail));
    }
    if (this->fl.head_phase == this->fl.tail_phase){
        assert(!(this->fl.head > this->fl.tail));
    }
}

void renamer::assert_active_list_invariance(){
    if (this->active_list_is_empty() || this->active_list_is_full()) return;
    if (this->al.head_phase != this->al.tail_phase){
        assert(!(this->al.head < this->al.tail));
    }
    if (this->al.head_phase == this->al.tail_phase){
        assert(!(this->al.head > this->al.tail));
    }
}

int renamer::free_list_regs_available(){
    if (this->free_list_is_full()) return this->free_list_size;
    if (this->free_list_is_empty()) return 0;
    
    uint64_t available = UINT64_MAX;
    if (this->fl.head_phase != this->fl.tail_phase){
        //otherwise inconsistent state: tail cannot be ahead of head,
        //means you are inserting entry when the list is already full
        assert(this->fl.head > this->fl.tail);
        available = this->fl.tail - this->fl.head + this->free_list_size;
        return available;
    }

    if (this->fl.head_phase == this->fl.tail_phase){
        //otherwise inconsistent state: head cant be ahead of tail, means
        // it allocated registers it does not have
        assert(this->fl.head < this->fl.tail);
        //available regsiters
        available = this->fl.tail - this->fl.head;
        return available;
    }


    return available; //it should never come here bc of the assertions 
}

uint64_t renamer::rename_rdst(uint64_t log_reg){
    uint64_t old = this->rmt[log_reg];
    uint64_t result = this->pop_free_list();

    if (result == UINT64_MAX){
        printf("FATAL ERROR: rename_rdst - not enough free list entry\n");
        exit(EXIT_FAILURE);
    } else {
        //update RMT //TODO: see if it's in AMT
        if (this->reg_in_rmt(result)){
            printf("%lu (popped from freelist) is already in RMT\n", result);
            exit(EXIT_FAILURE);
        }

        //if the popped reg from free list is not in RMT, assign it to RMT
        this->rmt[log_reg] = result; 
    }
   
    assert(old != this->rmt[log_reg]);
    if (this->reg_in_amt(result)){
            printf("%lu (popped from freelist) is already in AMT\n", result);
            exit(EXIT_FAILURE);
    }

    return result;
}

uint64_t renamer::allocate_gbm_bit(){
    //NOTE: Allocate free bit from left to right, so if i<j and both of bits
    //      are zero, it will return i

    uint64_t gbm = this->GBM;
    if (gbm == UINT64_MAX) return -1;
    
    uint64_t i;
    uint64_t n = this->num_checkpoints;
    uint64_t mask = 1;

    for (i=0; i < n; i++){
        if (gbm & mask){
            mask <<= 1;
        } else {
            return i;
        }
    }

    return UINT64_MAX; //No free bit found
}

uint64_t renamer::get_branch_mask(){
    //An instruction's initial branch mask is the value of the
    //the GBM when the instruction is renamed.

    return this->GBM;
}


uint64_t renamer::checkpoint(){
    //create a new branch checkpoint
    //allocate:
    //  1. GMB bit: starting from left or right? which way does this move? 
    //  2. Shadow Map Table (checkpointed RMT)
    uint64_t gbm_bit = this->allocate_gbm_bit();
    if ((gbm_bit < 0) | (gbm_bit > this->num_checkpoints)){
        printf("This should not happen. Could not allocate checkpoint, should stall\n");
        exit(EXIT_FAILURE);
    }

    //Setting the allocated bit and marking it unavialable
    this->GBM |= (1ULL<<gbm_bit);

    cp temp;
    // Shadow Map Table: copying over the AMT content
    temp.shadow_map_table = new uint64_t[this->shadow_map_table_size];
    uint64_t i;
    for (i=0; i < this->shadow_map_table_size; i++){
        temp.shadow_map_table[i] = this->rmt[i];
    }
    // SMT: head and head phase
    temp.free_list_head = this->fl.head;
    temp.free_list_head_phase = this->fl.head_phase;
    temp.gbm = this->GBM;

    this->checkpoints[gbm_bit] = temp;

    return gbm_bit;
}

uint64_t renamer::insert_into_active_list(){
    //if it's full, you cannot push more into it
    if (this->active_list_is_full()){
        return UINT64_MAX; 
    }

    uint64_t insertion_point = this->al.tail;

    this->al.tail++;
    if (this->al.tail == this->active_list_size){
        this->al.tail = 0;
        this->al.tail_phase = !this->al.tail_phase;
    }
  
    return insertion_point;
}

uint64_t renamer::dispatch_inst(bool dest_valid,
                           uint64_t log_reg,
                           uint64_t phys_reg,
                           bool load,
                           bool store,
                           bool branch,
                           bool amo,
                           bool csr,
                           uint64_t PC){
    /* Mechanism: Reserve entry at tail, write the instruction's logical
      and physical destination register specifiers, increment tail pointer
    */

    assert(!this->active_list_is_full());
    uint64_t idx_at_al = this->insert_into_active_list(); 
    if (idx_at_al == UINT64_MAX){
        printf("FATAL ERROR: cannot insert when active list is full\n");
        exit(EXIT_FAILURE);
    }

    //make a new active list entry
    al_entry_t *active_list_entry;
    active_list_entry = &this->al.list[idx_at_al]; //WIP

    active_list_entry->has_dest = dest_valid;
    if (dest_valid == true){
        active_list_entry->logical = log_reg;
        active_list_entry->physical = phys_reg;
    } else {
        active_list_entry->logical = UINT64_MAX;
        active_list_entry->physical = UINT64_MAX;
    }

    //the following flags should be false since they are just dispatched
    active_list_entry->completed = false;
    active_list_entry->exception = false;
    active_list_entry->br_mispredict = false;
    active_list_entry->val_mispredict = false;
    active_list_entry->load_violation = false;

    //whatever was passed to this function
    active_list_entry->is_load = load;
    active_list_entry->is_store = store;
    active_list_entry->is_branch = branch;
    active_list_entry->is_amo = amo;
    active_list_entry->is_csr = csr;
    active_list_entry->pc = PC;

    return idx_at_al;
}

bool renamer::stall_dispatch(uint64_t bundle_dst){
    //Assert Active List is not full
    uint64_t available_al_entry = this->get_free_al_entry_count();

    if (this->active_list_is_full()){
        return true;
    }
   
    if ((available_al_entry < bundle_dst) || (available_al_entry <= 0)){
        return true;
    }

    return false;
}

bool renamer::is_ready(uint64_t phys_reg){
    return this->prf_ready[phys_reg];
}

void renamer::clear_ready(uint64_t phys_reg){
    this->prf_ready[phys_reg] = 0;
}

void renamer::set_ready(uint64_t phys_reg){
    this->prf_ready[phys_reg] = 1;
}

uint64_t renamer::read(uint64_t phys_reg){
    return this->prf[phys_reg];
}

void renamer::write(uint64_t phys_reg, uint64_t value){
    this->prf[phys_reg] = value; 
}

void renamer::set_complete(uint64_t AL_index){
    this->al.list[AL_index].completed = true; 
}

bool renamer::stall_branch(uint64_t bundle_branch){
    uint64_t gbm = this->GBM;
    //simple use case, all taken, DO STALL
    if (gbm == UINT64_MAX) return true;

    uint64_t free_count=0;
    uint64_t n = num_checkpoints;
    uint64_t mask = 1;
    
    uint64_t i;
    for (i=0; i<n; i++){
        if (gbm & mask){
            mask <<= 1;
        } else {
            mask <<= 1;
            free_count++;
        }
    }

    //not enough space, DO STALL
    if (free_count < bundle_branch) return true;
    
    //free_count >= bundle_branch, enough space available. DONT STALL
    return false;
} 

bool renamer::precommit(bool &completed,
                        bool &exception, bool &load_viol, bool &br_misp,
                        bool &val_misp, bool &load, bool &store,
                        bool &branch, bool &amo, bool &csr,
                        uint64_t &PC){


    if (this->active_list_is_empty()){
        return false; 
    }

    al_entry_t *head;
    head = &this->al.list[this->al.head];

    completed = head->completed; 
    exception = head->exception;
    load_viol = head->load_violation;
    br_misp   = head->br_mispredict;
    val_misp  = head->val_mispredict;
    load      = head->is_load;
    store     = head->is_store;
    branch    = head->is_branch;
    amo       = head->is_amo;
    csr       = head->is_csr;
    PC        = head->pc;

    return true;
}

void renamer::commit(){
    assert(!this->active_list_is_empty());     
    al_entry_t *al_head;
    al_head = &this->al.list[this->al.head];

    //assert the following flags are correct
    assert(al_head->completed == true);
    assert(al_head->exception == false);
    assert(al_head->load_violation == false);

    //commit the instruction at the head of the active list
    //find the physical dst register by looking up the AMT with logical reg
    //EXCEPTION: only if the instruction has a valid destination
    bool op;
    if (al_head->has_dest == true){
        assert(this->free_list_is_full() != true);
        uint64_t old_mapping = this->amt[al_head->logical];
        //Update AMT with with new mapping 
        this->amt[al_head->logical] = al_head->physical;

        op = this->push_free_list(old_mapping);
        if (op == false){
            printf("FATAL ERROR: tried to push when the free list is full\n");
            exit(EXIT_FAILURE);
        }
    }

    op = this->retire_from_active_list();
    if (op == false){
        printf("FATAL ERROR: could not retire from AL since its empty\n");
        exit(EXIT_FAILURE);
    }

    return;
}

bool renamer::retire_from_active_list(){
    if (this->active_list_is_empty()){
        return false;
    }

    this->al.head++;
    if (this->al.head == this->active_list_size){
        //wrap around
        this->al.head = 0;
        this->al.head_phase = !this->al.head_phase;
    }
    
    return true;
}

void renamer::resolve(uint64_t AL_index, uint64_t branch_ID, bool correct){
    if (correct){ //branch was predicted correctly
        //clear the GBM bit by indexing with branch_ID
        this->GBM  &= ~(1ULL<<branch_ID);
        //clear all the checkpointed GBMs;
        uint64_t i;
        for (i=0; i < num_checkpoints; i++){
            this->checkpoints[i].gbm &= ~(1ULL<<branch_ID);
        } 

    } else {
        // In the case of a misprediction:
        // * Restore the GBM from the branch's checkpoint. Also make sure the
        //   mispredicted branch's bit is cleared in the restored GBM,
        //   since it is now resolved and its bit and checkpoint are freed.
        uint64_t misp_gbm = this->checkpoints[branch_ID].gbm;
        misp_gbm  &= ~(1ULL<<branch_ID);
        this->GBM = misp_gbm;

        // * Restore the RMT using the branch's checkpoint.
        uint64_t i;
        for (i=0; i < this->shadow_map_table_size; i++){
            this->rmt[i] = this->checkpoints[branch_ID].shadow_map_table[i]; 
        }
        // * Restore the Free List head pointer and its phase bit,
        //   using the branch's checkpoint.
        this->fl.head = this->checkpoints[branch_ID].free_list_head;
        this->fl.head_phase =this->checkpoints[branch_ID].free_list_head_phase;

        // * Restore the Active List tail pointer and its phase bit
        //   corresponding to the entry after the branch's entry.
        this->al.tail = AL_index + 1;
        // AL cannot be empty, so it has to be partially or completely full.
        if (this->al.tail == this->active_list_size){
            this->al.tail = 0;
        }

        // Inferring tail phase from head and tail pointers, w/ following logic
        // if hp == tp and h == t empty - not possible
        //    hp != tp and h == t full  - possible
        //    hp == tp     h  < t partly filled - possible
        //    hp != tp     h  > t possible 
        if (al.head == al.tail) al.tail_phase = !al.head_phase;
        if (al.head  > al.tail) al.tail_phase = !al.head_phase;
        if (al.head  < al.tail) al.tail_phase =  al.head_phase;

        //Make sure the restoration is correct
        this->assert_active_list_invariance();
    }
}


void renamer::squash(){
    //the renamer should be rolled back to the committed state of the machine
    uint64_t i;
    this->al.tail = this->al.head;
    this->al.tail_phase = this->al.head_phase;
    //empty out the active list, clear out AL contents.
    for (i=0; i < active_list_size; i++){
        init_al_entry(&al.list[i]);
    }
    
    this->restore_free_list();

    //copy AMT to RMT
    for (i=0; i < this->map_table_size; i++){
        rmt[i] = amt[i];
    }
    
    //Clear the checkpoints
    this->GBM = 0;
    for (i=0; i<num_checkpoints; i++){
        this->checkpoints[i].free_list_head = 0;
        this->checkpoints[i].free_list_head_phase = 0;
        this->checkpoints[i].gbm = 0;
        this->checkpoints[i].shadow_map_table = {0};
    } 

    return;
}

void renamer::set_exception(uint64_t AL_index){
    this->al.list[AL_index].exception = true;
}

void renamer::set_load_violation(uint64_t AL_index){
    this->al.list[AL_index].load_violation = true;
}

void renamer::set_branch_misprediction(uint64_t AL_index){
    this->al.list[AL_index].br_mispredict = true;
}

void renamer::set_value_misprediction(uint64_t AL_index){
    this->al.list[AL_index].val_mispredict = true;
}

bool renamer::get_exception(uint64_t AL_index){
    return this->al.list[AL_index].exception; 
}


int renamer::get_free_al_entry_count(){
    if (this->active_list_is_full()) return 0;
    if (this->active_list_is_empty()) return this->active_list_size;

    int used, free;
    if (this->al.head_phase == this->al.tail_phase){
        assert(this->al.tail > this->al.head);
        used = this->al.tail - this->al.head;
        free = this->active_list_size - used;
        return free;
    }
    else if (this->al.head_phase != this->al.tail_phase){
        assert(this->al.head > this->al.tail);
        free = this->al.head - this->al.tail; 
        return free;
    }

     // inconsistent state
    return -1;
}

bool renamer::active_list_is_full(){
    if ((this->al.head == this->al.tail) && 
        (this->al.head_phase != this->al.tail_phase)){

        return true;
    }

    return false;
}

bool renamer::active_list_is_empty(){
    if ((this->al.head == this->al.tail) && 
        (this->al.head_phase == this->al.tail_phase)){

        return true;
    }

    return false;
}


void renamer::init_al_entry(al_entry_t *ale){

    /* Initiate an Active List Entry with the correct type of values*/
    ale->has_dest=false;
    ale->logical=UINT64_MAX;
    ale->physical=UINT64_MAX;
    ale->completed=false;
    ale->exception=false;
    ale->load_violation=false;
    ale->br_mispredict=false;
    ale->val_mispredict=false;
    ale->is_load=false;
    ale->is_store=false;
    ale->is_branch=false;
    ale->is_amo=false;
    ale->is_csr=false;
    ale->pc=UINT64_MAX;
}


//Debugging helper functions
/*
void renamer::print_free_list(){
    uint64_t i=0;
    printf("--------------FREE LIST-------------------\n");
    while (i < free_list_size){
        printf("| %3llu ", fl.list[i]);
        i++;
    }
    printf("|\n");
    printf("------------END FREE LIST-----------------\n");
    printf("FL: tail: %d, tail_phase:%d, head: %d, head_phase: %d\n", 
            fl.tail, fl.tail_phase, fl.head, fl.head_phase);
}
void renamer::print_amt(){
    printf("---------------------AMT-----------------\n");
    for (int i=0; i < this->map_table_size; i++){
        printf("| %3d ", amt[i]);
    }
    printf("\n-------------------END_AMT-----------------\n");
}
void renamer::print_rmt(){
    printf("---------------------RMT-----------------\n");
    for (int i=0; i < this->map_table_size; i++){
        printf("| %3d ", rmt[i]);
    }
    printf("\n-------------------END_RMT-----------------\n");

}
void renamer::print_prf(){
    printf("---------------------PRF-----------------\n");
    for (int i=0; i < this->num_phys_reg; i++){
        printf("| %8llu ", prf[i]);
    }
    printf("\n-------------------END_PRF-----------------\n");

}
void renamer::print_prf_ready(){
    printf("---------------------PRF_READY-----------------\n");
    for (int i=0; i < this->num_phys_reg; i++){
        printf("| %llu ", prf_ready[i]);
    }
    printf("\n-------------------END_PRF_READY-----------------\n");

}
void renamer::print_active_list(bool between_head_and_tail){
    int i=0, n=active_list_size;
    if (between_head_and_tail) {
        i = this->al.head;
        n = this->al.tail;
    }
    if (n - i <= 0){
        i = 0;
        n = active_list_size;
        printf("ACTIVE LIST IS FULL. PRINTING ALL\n");
    }

    printf("| idx | log| phys | com | exc | dest | PC | _ret |\n");
    for (i; i < n; i++){
        al_entry_t *t;
        t = &this->al.list[i];
        printf("| %3d | %3d | %3d | %3d | %3d | %3d| %llu |\n",
                i,     t->logical, t->physical, t->completed,
                t->exception, t->has_dest, t->pc);
    }
    printf("AL Head: %d, AL tail: %d, Head Phase: %d, Tail Phase: %d\n",
            this->al.head, this->al.tail, this->al.head_phase, this->al.tail_phase
        );

}
*/

bool renamer::reg_in_amt(uint64_t phys_reg){
    uint64_t i;
    for (i=0; i < this->map_table_size; i++){
        if (this->amt[i] == phys_reg) return true;    
    }
    return false;
}

bool renamer::reg_in_rmt(uint64_t phys_reg){
    uint64_t i;
    for (i=0; i < this->map_table_size; i++){
        if (this->rmt[i] == phys_reg) return true;    
    }
    return false;
}
