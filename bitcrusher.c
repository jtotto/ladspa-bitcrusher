/*  Copyright 2013 Joshua Otto

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>. */

#include <ladspa.h>

#include <stdlib.h>
#include <math.h>
#include <string.h>

// Defining the plugin port numbers.  Both plugins are mono, and have one control each.
#define C_FACTOR 0
#define C_INPUT  1
#define C_OUTPUT 2

#define FLOAT_STEP 0x0.00001p0f // 2^-24, the step size of the single precision sample significands. (LADSPA_Data is just float in ladspa.h)
                                // This assumes IEEE 754 single precision floats, as does some of the processing code.
#define Q_FACTOR_LOWER 1.0f
#define Q_FACTOR_UPPER 0x1.0p21f // Any values higher than this silence the input signal (at least for practical purposes).

#define D_FACTOR_LOWER 1.0f
#define D_FACTOR_UPPER 300.0f // 

#define strdup homebrew_strdup // I discovered after writing that strdup isn't actually in the standard, and rolling my own seemed like a fun
                               // exercise.

char *homebrew_strdup(const char *in)
{
    char *duplicate = malloc((strlen(in) + 1) * sizeof(char));
    if(duplicate)
    {
        strcpy(duplicate, in);
    }
    return duplicate;
}

// Both plugins have the exact same internal data, so a lot of plugin infrastructure can be shared between the two plugins in the library!
// The difference between the two lies in the meaning of the reductionFactor member.

typedef struct {
    // Ports:
    LADSPA_Data *reductionFactor;
    LADSPA_Data *inputPort;
    LADSPA_Data *outputPort;

    // Run_Adding gain:
    LADSPA_Data runAddingGain;
} Crusher;

typedef Crusher Quantizer;
typedef Crusher Downsampler;

// There's really no preparation required.  Note that the activate/deactivate functions are pointless here and have been omitted.
LADSPA_Handle instantiateCrusher(const LADSPA_Descriptor *Descriptor, unsigned long sampleRate)
{
    return malloc(sizeof(Crusher));
}

// This is a trivial port connection function.
void connectPortToCrusher(LADSPA_Handle instance, unsigned long port, LADSPA_Data *DataLocation)
{
    Crusher *c_instance = (Crusher *) instance;

    switch(port)
    {
        case C_FACTOR:
            c_instance->reductionFactor = DataLocation;
            break;
        case C_INPUT:
            c_instance->inputPort = DataLocation;
            break;
        case C_OUTPUT:
            c_instance->outputPort = DataLocation;
            break;
    }
}

void setCrusherRunAddingGain(LADSPA_Handle instance, LADSPA_Data newGain)
{
    ((Quantizer *)instance)->runAddingGain = newGain;
}

/*---------- Quantizer ----------*/

// Surprisingly, math.h doesn't define a signum function.  I had planned to use bitwise operations assuming the IEEE754 standard,
// but discovered that this is fundamentally disallowed by the language.
float signum(float in)
{
    return in > 0.0f ? 1.0f : (in < 0.0f ? -1.0f : 0.0f); // I should probably use the ternary operator less.  
}

// Macros are used to allow the same code to be used for both run and run_adding.  
// This approach is also employed by the decimator plugin by Steve Harris (plugin.org.uk).
#undef buffer_write
#define buffer_write(b, v) (b = v)

void runQuantizer(LADSPA_Handle instance, unsigned long sampleCount)
{
    Quantizer *q_instance = (Quantizer *) instance;

    // Get the input and output ports.  The convention in all plugins I've read seems to be to assign these to local variables, instead of 
    // accessing the instance struct members each time.
    LADSPA_Data *input = q_instance->inputPort, *output = q_instance->outputPort;

    // Calculate the step size from the input value.
    float stepSize = (*(q_instance->reductionFactor) >= Q_FACTOR_LOWER && *(q_instance->reductionFactor) <= Q_FACTOR_UPPER)
                     ? *(q_instance->reductionFactor) * FLOAT_STEP : Q_FACTOR_LOWER;
    
    // Calculation intermediate storage.
    int exponentContainer;
    float significand;

    // Performing the processing.
    for(int i = 0; i < sampleCount; i++)
    {
        significand = frexpf(input[i], &exponentContainer); // Extract the significand of the sample for quantization.
        significand = signum(significand) * floorf(fabs(significand)/stepSize + 0.5f) * stepSize; // Apply the quantization! 
        buffer_write(output[i], ldexpf(significand, exponentContainer)); // Reapply the exponent and write the quantized value.
    }
}

// I hate duplicated code as much as the next person, but every LADSPA plugin I've ever read (including the ones included in the SDK)
// simply duplicate the regular run code with the adjustment in the following macro made.

#undef buffer_write
#define buffer_write(b, v) (b += (v) * runAddingGain)

void runAddingQuantizer(LADSPA_Handle instance, unsigned long sampleCount)
{
    Quantizer *q_instance = (Quantizer *) instance;

    // Get the input and output ports.  The convention in all of the plugins I've read has been to assign these to local variables, instead of 
    // accessing the instance struct members each time.
    LADSPA_Data *input = q_instance->inputPort, *output = q_instance->outputPort;

    // Get the gain for run_adding.  This is used by the macro!
    LADSPA_Data runAddingGain = q_instance->runAddingGain;

    // Calculate the step size from the input value.
    float stepSize = (*(q_instance->reductionFactor) >= Q_FACTOR_LOWER && *(q_instance->reductionFactor) <= Q_FACTOR_UPPER)
                     ? *(q_instance->reductionFactor) * FLOAT_STEP : Q_FACTOR_LOWER;
    
    // Calculation intermediate storage.
    int exponentContainer;
    float significand;

    // Performing the processing.
    for(int i = 0; i < sampleCount; i++)
    {
        significand = frexpf(input[i], &exponentContainer); // Extract the significand of the sample for quantization.
        significand = signum(significand) * floorf(fabs(significand)/stepSize + 0.5f) * stepSize; // Apply the quantization! 
        buffer_write(output[i], ldexpf(significand, exponentContainer)); // Reapply the exponent and write the quantized value.
    }
}

/*---------- Downsampler ------------*/

#undef buffer_write
#define buffer_write(b, v) (b = v)

void mean(float *data, unsigned long n, float *result)
{
    *result = 0.0f;
    for(unsigned long i = 0; i < n; i++)
    {
        *result += data[i];
    }
    *result /= n;
}

void runDownsampler(LADSPA_Handle instance, unsigned long sampleCount)
{
    Downsampler *d_instance = (Downsampler *) instance;

    // Get the input and output ports.  The convention in all plugins I've read seems to be to assign these to local variables, instead of 
    // accessing the instance struct members each time.
    LADSPA_Data *input = d_instance->inputPort, *output = d_instance->outputPort;

    // Get the downsampling factor as an integer.
    unsigned long reductionFactor = *(d_instance->reductionFactor) <= sampleCount ? *(d_instance->reductionFactor) : sampleCount, i;

    LADSPA_Data average;

    while(sampleCount > reductionFactor)
    {
        mean(input, reductionFactor, &average);
        for(i = 0; i < reductionFactor; i++)
        {
            buffer_write(output[i], average);
        }

        input += reductionFactor;
        output += reductionFactor;
        sampleCount -= reductionFactor;
    }
    mean(input, sampleCount, &average);
    for(i = 0; i < sampleCount; i++)
    {
        buffer_write(output[i], average);
    }
}

#undef buffer_write
#define buffer_write(b, v) (b += (v) * runAddingGain)

void runAddingDownsampler(LADSPA_Handle instance, unsigned long sampleCount)
{
    Downsampler *d_instance = (Downsampler *) instance;

    // Get the input and output ports.  The convention in all plugins I've read seems to be to assign these to local variables, instead of 
    // accessing the instance struct members each time.
    LADSPA_Data *input = d_instance->inputPort, *output = d_instance->outputPort;
    LADSPA_Data runAddingGain = d_instance->runAddingGain;

    // Get the downsampling factor as an integer.
    unsigned long reductionFactor = *(d_instance->reductionFactor) <= sampleCount ? *(d_instance->reductionFactor) : sampleCount, i;

    LADSPA_Data average;

    while(sampleCount > reductionFactor)
    {
        mean(input, reductionFactor, &average);
        for(i = 0; i < reductionFactor; i++)
        {
            buffer_write(output[i], average);
        }

        input += reductionFactor;
        output += reductionFactor;
        sampleCount -= reductionFactor;
    }
    mean(input, sampleCount, &average);
    for(i = 0; i < sampleCount; i++)
    {
        buffer_write(output[i], average);
    }
}

/*--------- Library overhead follows. ----------*/

void cleanupPlugin(LADSPA_Handle instance)
{
    free(instance);
}

// These global variables holds the unique plugin descriptor that is returned to any hosts requesting the descriptor information from the
// ladspa_descriptor function provided below (instead of constructing an arbitrary number of identical descriptors, one for each request).
LADSPA_Descriptor *g_qDescriptor;
LADSPA_Descriptor *g_dDescriptor;

// This function initializes the plugin descriptor when the plugin is first loaded.
void _init()
{
    char **portNames; // This is an array of strings, with one string for each port name.  These are the names used by the host.
    LADSPA_PortDescriptor *portDescriptors; // This is an array of LADSPA_PortDescriptors (ints, as typedef'd in ladspa.h), with each index
                                            // corresponding to one port.
    LADSPA_PortRangeHint *portRangeHints; // This is an array of LADSPA_PortRangeHint structs - again, one for each port.

    // Because the descriptors are very similar I could probably have implemented some sort of shared initialization function, but the 
    // clarity trade-off is in no way worth the utterly trivial gain.
    g_qDescriptor = malloc(sizeof(LADSPA_Descriptor)); // Allocate the quantizer descriptor struct.
    if(g_qDescriptor)
    {
        // Now to simply fill out the fields in the LADSPA_Descriptor struct.
        g_qDescriptor->UniqueID = 1337; // I HAVE NOT RESERVED THIS ID FROM THE CENTRAL LADSPA AUTHORITY.  Change to something more suitable
                                        // if this conflicts with your currently installed plugins.
        g_qDescriptor->Label = strdup("basic_quantizer");
        g_qDescriptor->Properties = LADSPA_PROPERTY_HARD_RT_CAPABLE; // Because the run functions have linear complexity and meet the other
                                                                     // requirements in the header, this plugin can be labelled RT.
        g_qDescriptor->Name = strdup("Quantizing Bitcrusher");
        g_qDescriptor->Maker = strdup("Joshua Otto");
        g_qDescriptor->Copyright = strdup("GPL");
        g_qDescriptor->PortCount = 3; // One control, one input, one output.
        
        portDescriptors = malloc(g_qDescriptor->PortCount * sizeof(LADSPA_PortDescriptor));
        portDescriptors[C_FACTOR] = LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL;
        portDescriptors[C_INPUT] = LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO;
        portDescriptors[C_OUTPUT] = LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO;
        g_qDescriptor->PortDescriptors = portDescriptors;

        portNames = malloc(g_qDescriptor->PortCount * sizeof(char *));
        portNames[C_FACTOR] = strdup("Quantization Factor");
        portNames[C_INPUT] = strdup("Input");
        portNames[C_OUTPUT] = strdup("Output");
        g_qDescriptor->PortNames = (const char **)portNames;

        portRangeHints = malloc(g_qDescriptor->PortCount * sizeof(LADSPA_PortRangeHint));
        portRangeHints[C_FACTOR].HintDescriptor = LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_MINIMUM
                                                                            | LADSPA_HINT_LOGARITHMIC;
        portRangeHints[C_FACTOR].LowerBound = Q_FACTOR_LOWER;
        portRangeHints[C_FACTOR].UpperBound = Q_FACTOR_UPPER;
        portRangeHints[C_INPUT].HintDescriptor = 0;
        portRangeHints[C_OUTPUT].HintDescriptor = 0;
        g_qDescriptor->PortRangeHints = portRangeHints;
        g_qDescriptor->instantiate = instantiateCrusher;
        g_qDescriptor->connect_port = connectPortToCrusher;
        g_qDescriptor->activate = NULL;
        g_qDescriptor->run = runQuantizer;
        g_qDescriptor->run_adding = runAddingQuantizer;
        g_qDescriptor->set_run_adding_gain = setCrusherRunAddingGain;
        g_qDescriptor->deactivate = NULL;
        g_qDescriptor->cleanup = cleanupPlugin;
    }
    
    g_dDescriptor = malloc(sizeof(LADSPA_Descriptor));
    if(g_dDescriptor)
    {
        g_dDescriptor->UniqueID = 1338; // I HAVE NOT RESERVED THIS ID FROM THE CENTRAL LADSPA AUTHORITY.  Change to something more suitable
                                        // if this conflicts with your currently installed plugins.
        g_dDescriptor->Label = strdup("basic_downsampler");
        g_dDescriptor->Properties = LADSPA_PROPERTY_HARD_RT_CAPABLE; // Because the run functions have linear complexity and meet the other
                                                                     // requirements in the header, this plugin can be labelled RT.
        g_dDescriptor->Name = strdup("Downsampling Bitcrusher");
        g_dDescriptor->Maker = strdup("Joshua Otto");
        g_dDescriptor->Copyright = strdup("GPL");
        g_dDescriptor->PortCount = 3; // One control, one input, one output.
        
        portDescriptors = malloc(g_dDescriptor->PortCount * sizeof(LADSPA_PortDescriptor));
        portDescriptors[C_FACTOR] = LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL;
        portDescriptors[C_INPUT] = LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO;
        portDescriptors[C_OUTPUT] = LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO;
        g_dDescriptor->PortDescriptors = portDescriptors;

        portNames = malloc(g_dDescriptor->PortCount * sizeof(char *));
        portNames[C_FACTOR] = strdup("Rate Reduction Factor");
        portNames[C_INPUT] = strdup("Input");
        portNames[C_OUTPUT] = strdup("Output");
        g_dDescriptor->PortNames = (const char **)portNames;

        portRangeHints = malloc(g_dDescriptor->PortCount * sizeof(LADSPA_PortRangeHint));
        portRangeHints[C_FACTOR].HintDescriptor = LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_MINIMUM;
        portRangeHints[C_FACTOR].LowerBound = D_FACTOR_LOWER;
        portRangeHints[C_FACTOR].UpperBound = D_FACTOR_UPPER;
        portRangeHints[C_INPUT].HintDescriptor = 0;
        portRangeHints[C_OUTPUT].HintDescriptor = 0;
        g_dDescriptor->PortRangeHints = portRangeHints;
        g_dDescriptor->instantiate = instantiateCrusher;
        g_dDescriptor->connect_port = connectPortToCrusher;
        g_dDescriptor->activate = NULL;
        g_dDescriptor->run = runDownsampler;
        g_dDescriptor->run_adding = runAddingDownsampler;
        g_dDescriptor->set_run_adding_gain = setCrusherRunAddingGain;
        g_dDescriptor->deactivate = NULL;
        g_dDescriptor->cleanup = cleanupPlugin;

    }
}

void deleteDescriptor(LADSPA_Descriptor *g_Descriptor)
{
    if(g_Descriptor)
    {
        free((char *)g_Descriptor->Label);
        free((char *)g_Descriptor->Name);
        free((char *)g_Descriptor->Maker);
        free((char *)g_Descriptor->Copyright);
        free((LADSPA_PortDescriptor *)g_Descriptor->PortDescriptors);
        for (int lIndex = 0; lIndex < g_Descriptor->PortCount; lIndex++)
          free((char *)(g_Descriptor->PortNames[lIndex]));
        free((char **)g_Descriptor->PortNames);
        free((LADSPA_PortRangeHint *)g_Descriptor->PortRangeHints);
        free(g_Descriptor);
    }
}
// This is taken pretty much verbatim from the SDK example plugins - basically just memory management overhead.
void _fini() 
{
    deleteDescriptor(g_qDescriptor);
    deleteDescriptor(g_dDescriptor);
}

// This returns a LADSPA_Descriptor of one of the plugins in this library.
const LADSPA_Descriptor *ladspa_descriptor(unsigned long index)
{
    switch(index)
    {
        case 0: return g_qDescriptor; break;
        case 1: return g_dDescriptor; break;
        default: return NULL;
    }
}
