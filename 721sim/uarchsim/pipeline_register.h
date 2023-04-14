#ifndef PIPELINE_REGISTER_H
#define PIPELINE_REGISTER_H

class pipeline_register {

public:

	bool valid;				              // valid instruction
	unsigned int index;			        // index into instruction payload buffer
	unsigned long long checkpoint_ID;	// branches that this instruction depends on

	pipeline_register();	// constructor

};

#endif //PIPELINE_REGISTER_H
