#include <ladspa.h>

#include <stdlib.h>
#include <math.h>
#include <string.h>

// Defining the plugin port numbers.  The plugin is mono and has only one control.
#define Q_FACTOR 0
#define Q_INPUT  1
#define Q_OUTPUT 2

#define FLOAT_STEP 0x0.00001p0f // 2^-24, the step size of the single precision sample significands. (LADSPA_Data is just float in ladspa.h)
                                // This assumes IEEE 754 single precision floats, as does some of the processing code.
#define FLOAT_SIGN_MASK 0x80000000   // Used to extract the sign, because apparently math.h doesn't have a signum function (what's the deal
                                     // with that anyway?) and I'm already assuming the IEEE 754 standard.

typedef struct {
    // Ports:
    LADSPA_Data *quantizationFactor;
    LADSPA_Data *inputPort;
    LADSPA_Data *outputPort;

    // Run_Adding gain:
    LADSPA_Data runAddingGain;
} Quantizer;

// There's really no preparation required.  Note that the activate/deactivate functions are pointless here and have been omitted.
LADSPA_Handle instantiateQuantizer(const LADSPA_Descriptor *Descriptor, unsigned long sampleRate)
{
    return malloc(sizeof(Quantizer));
}

// This is a trivial port connection function.
void connectPortToQuantizer(LADSPA_Handle instance, unsigned long port, LADSPA_Data *DataLocation)
{
    Quantizer q_instance = (Quantizer *) instance;

    switch(port)
    {
        case Q_FACTOR:
            q_instance->quantizationFactor = DataLocation;
            break;
        case Q_INPUT:
            q_instance->inputPort = DataLocation;
            break;
        case Q_OUTPUT:
            q_instance->outputPort = DataLocation;
            break;
    }
}

// Macros are used to allow the same code to be used for both run and run_adding.  
// This approach is also employed by the decimator plugin by Steve Harris (plugin.org.uk).
#undef buffer_write
#define buffer_write(b, v) (b = v)

void runQuantizer(LADSPA_Handle instance, unsigned long sampleCount)
{
    Quantizer *q_instance = (Quantizer *) instance;

    // Get the input and output ports.  The convention is all plugins I've read seems to be to assign these to local variables, instead of 
    // accessing the instance struct members each time.
    LADSPA_Data *input = q_instance->inputPort, output = q_instance->outputPort;

    // Calculate the step size from the input value.
    float stepSize = q_instance->quantizationFactor * FLOAT_STEP;
    
    // Calculation intermediate storage.
    int exponentContainer;
    float significand;
    const unsigned long SIGN_MASK = FLOAT_SIGN_MASK;

    // Performing the processing.
    for(int i = 0; i < sampleCount; i++)
    {
        significand = frexpf(input[i], &exponentContainer); // Extract the significand of the sample for quantization.
        significand = (significand & SIGN_MASK) | (floorf(fabs(significand)/stepSize + 0.5f) * stepSize; // Apply the quantization! 
        buffer_write(output[i], ldexpf(significand, exponentContainer)); // Reapply the exponent and write the quantized value.
    }
}

// I hate duplicated code as much as the next person, but every LADSPA plugin I've ever read (including the ones included in the SDK)
// simply duplicate the regular run code with the adjustment in the following macro made.

#undef buffer_write
#define buffer_write(b, v) (b += (v) * runAddingGain)

void setQuantizerRunAddingGain(LADSPA_Handle instance, LADSPA_Data newGain)
{
    ((Quantizer *)instance)->runAddingGain = newGain;
}

void runAddingQuantizer(LADSPA_Handle instance, unsigned long sampleCount)
{
    Quantizer *q_instance = (Quantizer *) instance;

    // Get the input and output ports.  The convention in all of the plugins I've read has been to assign these to local variables, instead of 
    // accessing the instance struct members each time.
    LADSPA_Data *input = q_instance->inputPort, output = q_instance->outputPort;

    // Get the gain for run_adding.  This is used by the macro!
    LADSPA_Data runAddingGain = q_instance->runAddingGain;

    // Calculate the step size from the input value.
    float stepSize = q_instance->quantizationFactor * FLOAT_STEP;
    
    // Calculation intermediate storage.
    int exponentContainer;
    float significand;
    const unsigned long SIGN_MASK = FLOAT_SIGN_MASK;

    // Performing the processing.
    for(int i = 0; i < sampleCount; i++)
    {
        significand = frexpf(input[i], &exponentContainer); // Extract the significand of the sample for quantization.
        significand = (significand & SIGN_MASK) | (floor(fabs(significand)/stepSize + 0.5f) * stepSize; // Apply the quantization! 
        buffer_write(output[i], ldexpf(significand, exponentContainer)); // Reapply the exponent and write the quantized value.
    }
}

void cleanupQuantizer(LADSPA_Handle instance)
{
    free(instance);
}

// This global variable holds the unique plugin descriptor that is returned to any hosts requesting the descriptor information from the
// ladspa_descriptor function provided below (instead of constructing an arbitrary number of identical descriptors, one for each request).
LADSPA_Descriptor *g_qDescriptor;

// This function initializes the plugin descriptor when the plugin is first loaded.
void _init()
{
    char **portNames; // This is an array of strings, with one string for each port name.  These are the names used by the host.
    LADSPA_PortDescriptor *portDescriptors; // This is an array of LADSPA_PortDescriptors (ints, as typedef'd in ladspa.h), with each index
                                            // corresponding to one port.
    LADSPA_PortRangeHint *portRangeHints; // This is an array of LADSPA_PortRangeHint structs - again, one for each port.

    g_qDescriptor = malloc(sizeof(LADSPA_Descriptor)); // Allocate the descriptor struct.
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
        portDescriptors[Q_FACTOR] = LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL;
        portDescriptors[Q_INPUT] = LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO;
        portDescriptors[Q_OUTPUT] = LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO;
        g_qDescriptor->PortDescriptors = portDescriptors;

        portNames = malloc(g_qDescriptor->PortCount * sizeof(char *));
        portNames[Q_FACTOR] = strdup("Quantization Factor");
        portNames[Q_INPUT] = strdup("Input");
        portNames[Q_OUTPUT] = strdup("Output");
        g_qDescriptor->PortNames = portNames;

        portRangeHints = malloc(g_qDescriptor->PortCount * sizeof(LADSPA_PortRangeHint));
        portRangeHints[Q_FACTOR].HintDescriptor = LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_LOW
                                                                            | LADSPA_HINT_LOGARITHMIC;
        portRangeHints[Q_FACTOR].LowerBound = 1.0f
        portRangeHints[Q_FACTOR].UpperBound = 0x1.0p24f;
        portRangeHints[Q_INPUT].HintDescriptor = 0;
        portRangeHints[Q_OUTPUT].HintDescriptor = 0;
        g_qDescriptor->PortRangeHints = portRangeHints;
        g_qDescriptor->instantiate = instantiateQuantizer;
        g_qDescriptor->connectPort = connectPortToQuantizer;
        g_qDescriptor->activate = NULL;
        g_qDescriptor->run = runQuantizer;
        g_qDescriptor->run_adding = runAddingQuantizer;
        g_qDescriptor->set_run_adding_gain = setQuantizerRunAddingGain;
        g_qDescriptor->deactivate = NULL;
        g_qDescriptor->cleanup = cleanupQuantizer;
    }
}

// This is taken pretty much verbatim from the SDK example plugins - basically just memory management overhead.
void _fini() 
{
    if (g_qDescriptor) 
    {
        free((char *)g_qDescriptor->Label);
        free((char *)g_qDescriptor->Name);
        free((char *)g_qDescriptor->Maker);
        free((char *)g_qDescriptor->Copyright);
        free((LADSPA_PortDescriptor *)g_qDescriptor->PortDescriptors);
        for (int lIndex = 0; lIndex < g_qDescriptor->PortCount; lIndex++)
          free((char *)(g_qDescriptor->PortNames[lIndex]));
        free((char **)g_qDescriptor->PortNames);
        free((LADSPA_PortRangeHint *)g_qDescriptor->PortRangeHints);
        free(g_qDescriptor);
    }
}

// This returns the LADSPA_Descriptor of the single plugin in this 'library.'
const LADSPA_Descriptor *ladspa_descriptor(unsigned long index)
{
    return index == 0 ? g_qDescriptor : NULL;
}
