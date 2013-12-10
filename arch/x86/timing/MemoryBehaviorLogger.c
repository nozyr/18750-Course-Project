#include <lib/mhandle/mhandle.h>
#include "lib/util/class.h"
#include <arch/x86/emu/context.h>
#include "thread.h"
#include "MemoryBehaviorLogger.h"

void x86ThreadInitMemorySummary(X86Context *self)
{
	struct x86_MRU_pattern_t *mru_i_pattern_logger = self->MemorySummary.MRU_Instruction_log;
    struct x86_MRU_pattern_t *mru_d_pattern_logger = self->MemorySummary.MRU_Data_log;
    struct x86_stride_pattern_t *stride_pattern_logger = self->MemorySummary.stride_pattern_log;

	// self->memlogger = (struct x86_mem_behavr_logger_t *) xcalloc(1, sizeof(struct x86_mem_behavr_logger_t));

	for (int index = 0; index < MAX_PATTERN_COUNT; ++index)
	{

		for (int way = 0; way < MRU_ASSOCIATIVITY; ++way)
		{

			mru_i_pattern_logger[index].tag[way] = 0;
			mru_i_pattern_logger[index].counter[way] = way;

            mru_d_pattern_logger[index].tag[way] = 0;
            mru_d_pattern_logger[index].counter[way] = way;

            stride_pattern_logger[index].instruction_address_count = 0;
            stride_pattern_logger[index].stride = 0;
            stride_pattern_logger[index].InitialAddress = 0;

		}
	}
}

void x86ThreadFreeMemorySummary(X86Context *self)
{
	// free(self->memlogger);
}

/*Yurui Insert stride pattern into Memory behavior logger*/
void X86InsertInMBL(X86Thread *self, unsigned int address, Patterns pattern)
{
	X86Context *Context = self->ctx;
	struct x86_mem_behavr_logger_t *mem_behav_log = &self->memlogger;

	int index = (address >> ADDRESS_INDEX_SHIFT) % BUFFER_INDEX_SIZE;

	switch(pattern)
	{
		case DATA_Pattern:
		{
			struct x86_mem_behavr_buffer *buffer = mem_behav_log->buffer;
			struct x86_stride_pattern_t *stride_pattern_logger = Context->MemorySummary.stride_pattern_log;
            struct x86_MRU_pattern_t *MRU_Data_logger = Context->MemorySummary.MRU_Data_log;
			int addressCount = buffer[index].Count;

			// FILE * pfile;

			buffer[index].Count = (buffer[index].Count + 1) % BUFFER_LENGTH;

			// assert(Count < BUFFER_LENGTH);

			// pfile = fopen("/home/witan/multi2sim-4.2/samples/x86/Trace.txt", "a");

			buffer[index].address[addressCount]= address;

			// fprintf(stderr, "Insert Address %d into buffer index %d\n", address, index);

			if (addressCount != BUFFER_LENGTH - 1)
			{
				return;
			}

			long temp_difference = 0;
			long difference = 0;

			int stride_pattern_count = 1;
			int stride_pattern_end = 0;
			int stride_pattern_max_length = 2;


			/*locate longest stride pattern within buffer*/
			for (int i = 0; i < BUFFER_INDEX_SIZE-2; ++i)
			{
				temp_difference = buffer[index].address[i+1] - buffer[index].address[i];
				if (difference != temp_difference)
				{
					difference = temp_difference;
					stride_pattern_count = 1;
				}
				else
				{
					stride_pattern_count++;
					if (stride_pattern_count > stride_pattern_max_length)
					{
						stride_pattern_max_length = stride_pattern_count;
						stride_pattern_end = i+1;
					}
				}
			}

			/*if address matches stride pattern, add the address into stride pattern logger*/
			if (stride_pattern_end != 0)
			{
				stride_pattern_logger[index].stride = difference;
				stride_pattern_logger[index].InitialAddress = buffer[index].address[stride_pattern_end - stride_pattern_max_length];
				stride_pattern_logger[index].instruction_address_count = stride_pattern_max_length;

				// fprintf(stderr, "Stride pattern with index %d update with stride %d, InitialAddress %d and length %d \n",
				// 	index, stride_pattern_logger[index].stride, stride_pattern_logger[index].InitialAddress,
				// 	stride_pattern_logger[index].instruction_address_count);

			}

            /*MRU Data Record*/
            for (int i = 0; i < BUFFER_INDEX_SIZE; ++i)
            {
                int Found_way = -1;
                for (int way = 0; way < MRU_ASSOCIATIVITY; ++way)
                {
                    /*Invalid Way Found*/
                    if(!MRU_Data_logger[index].tag[way])
                    {
                        MRU_Data_logger[index].tag[way] = address;
                        break;
                    }

                    /*hit*/
                    if ((MRU_Data_logger[index].tag[way] >> ADDRESS_INDEX_SHIFT) == (address >> ADDRESS_INDEX_SHIFT))
                    {
                        break;
                    }
                }

                if (Found_way == -1)
                {
                    for (int way = 0; way < MRU_ASSOCIATIVITY; ++way)
                    {
                        MRU_Data_logger[index].counter[way]--;

                        if (MRU_Data_logger[index].counter[way] < 0)
                        {
                            MRU_Data_logger[index].counter[way] = MRU_ASSOCIATIVITY - 1;
                            MRU_Data_logger[index].tag[way] = address;
                        }
                    }
                }

            }

			break;
		}

		case Instructioin_Pattern:
		{
			struct x86_MRU_pattern_t *mru_i_pattern_logger = Context->MemorySummary.MRU_Instruction_log;
			int Found_way = -1;

			for (int way = 0; way < MRU_ASSOCIATIVITY; ++way)
			{
				/*Invalid Way Found*/
				if(!mru_i_pattern_logger[index].tag[way])
				{
					mru_i_pattern_logger[index].tag[way] = address;
					break;
				}

				/*hit*/
				if ((mru_i_pattern_logger[index].tag[way] >> ADDRESS_INDEX_SHIFT) == (address >> ADDRESS_INDEX_SHIFT))
				{
					break;
				}
			}

			if (Found_way == -1)
			{
				for (int way = 0; way < MRU_ASSOCIATIVITY; ++way)
				{
					mru_i_pattern_logger[index].counter[way]--;

					if (mru_i_pattern_logger[index].counter[way] < 0)
					{
						mru_i_pattern_logger[index].counter[way] = MRU_ASSOCIATIVITY - 1;
						mru_i_pattern_logger[index].tag[way] = address;
					}
				}
			}
            break;
		}

        default:
            break;
	}
	// fclose(pfile);
}
