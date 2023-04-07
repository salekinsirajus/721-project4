#define __STDC_FORMAT_MACROS

#include "renamer.h"
#include <cassert>

renamer::renamer(uint64_t n_log_regs,
        uint64_t n_phys_regs,
        uint64_t n_chkpts,
        uint64_t n_active){
    
    //Run the assertions
    assert(n_phys_regs > n_log_regs);
    //assert((0 <= n_branches) && (n_branches <= 64)); //TODO: is there anything like that for CPR
    assert(n_active > 0);
  
    //initialize the data structures
    map_table_size = n_log_regs;
    num_phys_reg = n_phys_regs;
    rmt = new uint64_t[n_log_regs];
    prf = new uint64_t[n_phys_regs];
    prf_ready = new uint64_t[n_phys_regs];
    prf_usage_counter = new uint64_t[n_phys_regs];
    prf_unmapped = new uint64_t[n_phys_regs];

    uint64_t j;
    //AMT and RMT should have the same value at the beginning
    //However, not sure what would the content be. if amt[0] = 0, amt[1] = 0
    //then all the logical registers are mapped to p0. OTOH, amt[0] = 0 and
    //amt[1] = 1,..., amt[n] = n indicate r0->p0, r1->p1,..
    //the contents of the prf does not matter I suppose.
    for (j=0; j < n_log_regs; j++){
        rmt[n_log_regs - 1 - j] = j;
    }

    //initializing the physical register files to be consistent with
    //an empty pipeline state
    for (j=0; j < n_phys_regs; j++){
        prf_ready[j] = 1;
        if (reg_in_rmt(j)){
            prf_unmapped[j] = 0;
        } else {
            prf_unmapped[j] = 1;
        }
        prf_usage_counter[j] = 0;
    }


    //checkpoint buffer
    chkpt_buffer_head = 0;
    chkpt_buffer_tail = 0;
    chkpt_buffer_head_phase = 0;
    chkpt_buffer_tail_phase = 0;
    num_checkpoints = n_chkpts;  //CHANGE THIS TO COMMAND LINE ARG LATER

    checkpoint_buffer = new chkpt[num_checkpoints];
    uint64_t i;
    for (i = 0; i < num_checkpoints; i++){
        checkpoint_buffer[i].rmt = new uint64_t[n_log_regs];
        checkpoint_buffer[i].unmapped_bits = new uint64_t[n_phys_regs];
        checkpoint_buffer[i].load_counter = 0;
        checkpoint_buffer[i].store_counter = 0;
        checkpoint_buffer[i].branch_counter = 0;
    }

    //populating the first checkpoint
    uint64_t k;
    for (k=0; k < n_log_regs; k++){
        checkpoint_buffer[chkpt_buffer_head].rmt[k] = rmt[k];
        //increameting the usage counter of the prf since we're checkpointing
        //the RMT at this location
        prf_usage_counter[rmt[k]] += 1;
    }

    for (j=0; j < n_phys_regs; j++){
        checkpoint_buffer[chkpt_buffer_head].unmapped_bits[j] = prf_unmapped[j];
    }

    //move the checkpoint tail ahead
    chkpt_buffer_tail++;

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
    for (i=0; i <free_list_size; i++){
        fl.list[i] = n_log_regs + i;
    }

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

bool renamer::checkpoint_buffer_is_empty(){
    if ((this->chkpt_buffer_head == this->chkpt_buffer_tail) && 
        (this->chkpt_buffer_head_phase == this->chkpt_buffer_tail_phase)){

        return true;
    }

    return false;
}

bool renamer::checkpoint_buffer_is_full(){
    if ((this->chkpt_buffer_head == this->chkpt_buffer_tail) && 
        (this->chkpt_buffer_head_phase != this->chkpt_buffer_tail_phase)){

        return true;
    }

    return false;
    
}

uint64_t renamer::get_free_checkpoint_count(){
    if (checkpoint_buffer_is_full()) return 0;
    if (checkpoint_buffer_is_empty()) return num_checkpoints;
     
    int used, free;
    if (this->chkpt_buffer_head_phase == this->chkpt_buffer_tail_phase){
        assert(this->chkpt_buffer_tail > this->chkpt_buffer_head);
        used = this->chkpt_buffer_tail - this->chkpt_buffer_head;
        free = this->num_checkpoints - used;
        return free;
    }

    else if (this->chkpt_buffer_head_phase != this->chkpt_buffer_tail_phase){
        assert(this->chkpt_buffer_head > this->chkpt_buffer_tail);
        free = this->chkpt_buffer_head - this->chkpt_buffer_tail; 
        return free;
    }

    // inconsistent state
    return -1; 
}

bool renamer::stall_checkpoint(uint64_t bundle_chkpt){
    //Get the number of available checkpoints
    if (bundle_chkpt > get_free_checkpoint_count()){
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

    return result;
}


void renamer::checkpoint(){
    //TODO: reimplement for CPR
    //checkpoint the current RMT 
    //find the insertion point of the checkpoint buffer
    uint64_t x = chkpt_buffer_tail;
    uint64_t i;
    for(i = 0; i < map_table_size; i++){
        checkpoint_buffer[x].rmt[i] = rmt[i];
        prf_usage_counter[rmt[i]] += 1;
    }

    checkpoint_buffer[x].load_counter = 0;
    checkpoint_buffer[x].store_counter = 0;
    checkpoint_buffer[x].branch_counter = 0;

    uint64_t j;
    for (j=0; j < num_phys_reg; j++){
        checkpoint_buffer[x].unmapped_bits[j] = prf_unmapped[j];
    }

    //advancing the tail 
    chkpt_buffer_tail++;
    if (chkpt_buffer_tail == num_checkpoints){
        chkpt_buffer_tail = 0;
        chkpt_buffer_tail_phase = !chkpt_buffer_tail_phase;
    }
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
    //TODO: reimplement for CPR
}


bool renamer::precommit(bool &completed,
                        bool &exception, bool &load_viol, bool &br_misp,
                        bool &val_misp, bool &load, bool &store,
                        bool &branch, bool &amo, bool &csr,
                        uint64_t &PC){

    //TODO: reimplement for CPR
    return true;
}

void renamer::commit(){
    //TODO: reimplement for CPR
    return;
}


void renamer::resolve(uint64_t AL_index, uint64_t branch_ID, bool correct){
    //TODO: reimplement for CPR
}


void renamer::squash(){
    //the renamer should be rolled back to the committed state of the machine
    //TODO: reimplement for CPR
    uint64_t i;
    
    this->restore_free_list();

    return;
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

bool renamer::reg_in_rmt(uint64_t phys_reg){
    uint64_t i;
    for (i=0; i < this->map_table_size; i++){
        if (this->rmt[i] == phys_reg) return true;    
    }
    return false;
}
