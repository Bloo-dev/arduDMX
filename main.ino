#include <AudioAnalyzer.h>
#include <Conceptinetics.h>
#include <DMXFixture.h>

// ===== GLOBAL SETTINGS ======
// Light Fixture Data
const uint8_t maxBrightness = 217;                                                                                   // 85% max brightness to increase LED lifetime
DMXFixture fixtures[] = {DMXFixture(1, maxBrightness), DMXFixture(7, maxBrightness), DMXFixture(13, maxBrightness)}; // configured fixtures and their start channels. The maximum amount of supported fixtures is 16.
const uint32_t colorProfiles[] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFF9000};                                           // colors for the configured fixtures to start out with, in order, stored in hex. Should be normalized to avoid differing fixture brightness values from fixture to fixture. The maximum amount of supported colors is 16.
const uint32_t frequencyProfiles[] = {0x00000F0, 0x00F0000, 0xFF00000, 0x000FF00};                                  // frequency responses of the fixtures stored in hex. Each digit corresponds to a frequency band, meaning each frequency band can have a response between 15 (max) and 0 (min). Leftmost digits are highest frequencies. The maximum amount of supported response values is 16.
// MSGEQ7 Signal Data
const uint8_t samplesPerRun = 16;       // number of consecutive samples to take whenever the audio is sampled (these are then averaged). Higher values inhibit random noise spikes.
const uint16_t delayBetweenSamples = 1; // time in ms to wait between samples in a consecutive sample run. High values will decrease temporal resolution drastically.
//  =============================

// ===== GLOBAL VARIABLES ======
// Fixture Management
const uint8_t fixtureAmount = sizeof(fixtures) / sizeof(DMXFixture);
const uint8_t fixtureProfileAmount = min(sizeof(colorProfiles) / sizeof(int), sizeof(frequencyProfiles) / sizeof(int)); // amount of fixture modes is the amount of (color Response, frequencyResponse) pairs. Ignore additional color or frequency responses.
// DMX Hardware
DMX_Master dmxMaster(fixtures[0].channelAmount * fixtureAmount, 2);
// FFT Hardware
Analyzer MSGEQ7 = Analyzer(6, 7, 0);
uint16_t frequencyAmplitudes[7]; // stores data from MSGEQ7 chip
// Auto Gain
uint16_t amplitudeHistory[64]; // stores the history of the cross-band average amplitude
uint8_t amplitudeHistoryEntry = 0;
uint16_t clippingHistory[64]; // stores the history of the cross-band clipping duty cycle
uint8_t clippingHistoryEntry = 0;
const uint8_t targetDutyCycle = 196.0;        // target value for fixture duty cycle (time-clipped/time-not-clipped)
const float amplificationFactorMax = 32;      // maximum allowed amplifaction factor
const float amplificationFactorMin = 0.03125; // minimal allowed amplification factor
float amplificationFactor = 12.0;             // amplification for signals considered non-noise (ones that should result in a non-zero light response), managed automatically
uint16_t noiseLevel = 0;                      // lower bound for noise, determined automatically at startup
// =============================
// Backup settings for basement: noiseLevel=200, signalAmplification=6.0

void setup()
{
    // Start FFT
    MSGEQ7.Init();

    // Start DMX
    dmxMaster.setAutoBreakMode();
    dmxMaster.enable();

    // Initialize Light Fixtures
    for (uint8_t fixtureId = 0; fixtureId < fixtureAmount; fixtureId++)
    {
        fixtures[fixtureId].reset(); // reset to default values
    }

    // Analyze Noise Levels (THERE MUST NOT BE AUDIO ON THE JACK FOR THIS TO WORK)
    int noiseData[] = {0, 0, 0, 0, 0, 0, 0};
    sampleMSGEQ7(32, 1, noiseData);
    noiseLevel = getAverage(noiseData, 7, 12); // average over all frequencies and add some extra buffer

    delay(500); // wait a bit for everything to stabalize
}

void loop()
{
    // get FFT data from MSGEQ7 chip
    sampleMSGEQ7(samplesPerRun, delayBetweenSamples, frequencyAmplitudes);

    // Store history of average signal across all bands (with background noise already subtracted)
    uint16_t signalMean = updateHistory(amplitudeHistory, 64, &amplitudeHistoryEntry, max((int32_t)getAverage(frequencyAmplitudes, 7, 0) - noiseLevel, 0));

    // Store history of duty cycle and transform values in frequencyAmplitudes into range [0..255]
    uint16_t dutyCycleMean = updateHistory(clippingHistory, 64, &clippingHistoryEntry, transformAudioSignal(noiseLevel + signalMean, amplificationFactor, frequencyAmplitudes));

    // Get new amplification factor based on duty cycle mean (duty cycle of 196 is about 19.1%)
    float dutyCycleDeviance = targetDutyCycle / dutyCycleMean;
    amplificationFactor = constrain(dutyCycleDeviance, amplificationFactorMin, amplificationFactorMax);
    // TODO add toggle for this for manual gain control

    // Cycle Fixtures
    uint32_t permutatedColorProfiles[fixtureAmount];
    uint32_t permutatedFrequencyProfiles[fixtureAmount];
    permutateProfiles(0xFEDCBA98, 0x76543210, colorProfiles, permutatedColorProfiles, frequencyProfiles, permutatedFrequencyProfiles, fixtureAmount);

    // Manage Fixtures
    for (uint8_t fixtureId = 0; fixtureId < fixtureAmount; fixtureId++)
    {
        setFixtureColor(fixtures[fixtureId], frequencyAmplitudes, permutatedColorProfiles[fixtureId]);
        setFixtureBrightness(fixtures[fixtureId], frequencyAmplitudes, permutatedFrequencyProfiles[fixtureId]);

        // send data to fixtures
        fixtures[fixtureId].display(dmxMaster);
    }

    delay(50); // wait a bit to give lights some on-time TODO adjust this timing
}

/**
 * @brief Appends a new entry to a history array, and returns the average of the array.
 *
 * @param targetArray The history array to modify.
 * @param historyLength Length of the history array.
 * @param currentSlot Adress of the variable that keeps track of the current slot to be modified, i.e. the oldest entry in the history array.
 * @param valueToWrite The value to be written into slot 'currentSlot'.
 * @return uint16_t average of the history array.
 */
uint16_t updateHistory(uint16_t *targetArray, uint8_t historyLength, uint8_t *currentSlot, uint16_t valueToWrite)
{
    targetArray[(*currentSlot)++] = valueToWrite;
    if ((*currentSlot) > (historyLength - 1))
        (*currentSlot) = 0;
    return getAverage(targetArray, historyLength, 0);
}

/**
 * @brief Gets the average value of an array.
 *
 * @param array Array of values to be averaged.
 * @param elements [0..63] The amount of elements in the array.
 * @param buffer [0..1023] Buffer value to be added onto the average after calculation.
 * @return Arithmetic average of the signal levels on all 7 bands plus the buffer value. Capped at 1023.
 */
uint16_t getAverage(int *array, uint16_t elements, uint16_t buffer)
{
    uint16_t sum = 0;
    for (int i = 0; i < elements; i++)
    {
        sum += array[i];
    }

    sum = buffer + (sum / elements);
    return min(sum, 1023);
}

/**
 * @brief Transforms a given 12-bit audio signal to an 8-bit signal that can be used to control DMXFixtures. Also performs some cleanup on the signal, like removing noise and scaling the signal to use the entire 8-bit space.
 *
 * @param lowSignalCutOff [0..1023] Signals lower or equal than this will be forced to 0. Applied before amplification.
 * @param amplificationFactor [0.0..10.0] (recommended) Multiplicative amplification factor to be applied to the signal.
 * @param targetArray The array holding the audio data to be modified. Should have seven (7) entries.
 *
 * @return [0..1023] The duty cycle of the average over all frequency bands in terms of clipping, i.e. how often the signal clipped the upper signal limit of 1023/255.
 */
uint16_t transformAudioSignal(uint16_t lowSignalCutOff, float amplificationFactor, int *targetArray)
{
    uint16_t bandClippings[] = {0, 0, 0, 0, 0, 0, 0};
    for (uint8_t band = 0; band < 7; band++)
    {
        int32_t rawSensorData = targetArray[band] - lowSignalCutOff;
        uint16_t level = amplificationFactor * max(rawSensorData, 0); // shift signal down, removing noise and static parts of the signal
        if (level >= 1023)
        {
            bandClippings[band] = 1023; // remember the signal clipped
        }

        level = level/4;
        targetArray[band] = (int)min(level, 255); // scale to [0..255] for use in light fixtures
    }

    return getAverage(bandClippings, 7, 0);
}

/**
 * @brief Gets values from all seven bands provided by the MSGEQ7 spectrum analyzer chip and stores the results for all seven bands to the target array.
 *
 * @param sampleAmount [0..63] The amount of samples to be taken. For each band, the samples taken will be summed up and averaged.
 * @param sampleDelay [0..65535] Delay between taking samples. This is added on-top of the run time of a sample capture.
 * @param targetArray The array to store the resulting data to. Should have at least seven elements.
 */
void sampleMSGEQ7(int8_t sampleAmount, uint16_t sampleDelay, int *targetArray)
{
    uint16_t averageAmplitudes[] = {0, 0, 0, 0, 0, 0, 0};
    for (uint8_t sample_count = 0; sample_count < sampleAmount; sample_count++)
    {
        uint16_t sampleAmplitudes[] = {0, 0, 0, 0, 0, 0, 0};
        MSGEQ7.ReadFreq(sampleAmplitudes); // store amplitudes of frequency bands into array
                                           // Frequency(Hz):        63  160  400  1K  2.5K  6.25K  16K
                                           // frequencyAmplitudes[]: 0    1    2   3     4      5    6

        averageAmplitudes[0] += sampleAmplitudes[0]; // sum up samples
        averageAmplitudes[1] += sampleAmplitudes[1];
        averageAmplitudes[2] += sampleAmplitudes[2];
        averageAmplitudes[3] += sampleAmplitudes[3];
        averageAmplitudes[4] += sampleAmplitudes[4];
        averageAmplitudes[5] += sampleAmplitudes[5];
        averageAmplitudes[6] += sampleAmplitudes[6];

        delay(sampleDelay); // wait before acquisition of next sample
    }

    targetArray[0] = averageAmplitudes[0] / sampleAmount; // calculate averages and store to target array
    targetArray[1] = averageAmplitudes[1] / sampleAmount;
    targetArray[2] = averageAmplitudes[2] / sampleAmount;
    targetArray[3] = averageAmplitudes[3] / sampleAmount;
    targetArray[4] = averageAmplitudes[4] / sampleAmount;
    targetArray[5] = averageAmplitudes[5] / sampleAmount;
    targetArray[6] = averageAmplitudes[6] / sampleAmount;
}

/**
    @brief Transforms the response tables to cycle colors or change frequency response.
    This is done via instructions supplied via the first three parameters, which, together build a 96-bit instruction, consisting of 16 6-bit instructions.
    Each 6-bit instruction is structured as: 0b(0000)[00] where the 2 bits in square brackets specify the instruction type,
    and the 4 bits in the round brackets specify the target fixture this instruction should apply to.
    The source fixture (need for some operations) is dependent on the position of the 6-bit instruction within the 12 byte instruction.
*/
void permutateProfiles(uint32_t permutationHigh, uint32_t permutationLow, uint32_t *constColorProfiles, uint32_t *permutatedColorProfiles, uint32_t *constFrequencyProfiles, uint32_t *permutatedFrequencyProfiles, uint16_t fixtureAmount)
{
    // automatically cycle instructions by one to make sure fixture see equal usage
    //uint8_t underflow = permutationLow & 0b1111; // TODO built 0xF with as many F as the phase
    //permutationLow = (permutationLow >> (4*phase)) + ((permutationHigh & 0b1111) << (28*phase));
    //permutationHigh = (permutationHigh >> (4*phase)) + (underflow << (28*phase));

    // store shuffled profiles into arrays for the fixtures to read from.
    // Note that these arrays only require a length of fixtureAmount, as any additional profiles will not be displayed on a fixture anyways.
    for (uint8_t profileSlot = 0; profileSlot < fixtureAmount; profileSlot++)
    {
        // extract lowest instruction
        uint8_t profileSource = (permutationLow & 0b1111);

        // shift remaining instructions
        permutationLow = (permutationLow >> 4) + ((permutationHigh & 0b1111) << 28);
        permutationHigh = (permutationHigh >> 4);

        // store to shuffled profile
        permutatedColorProfiles[profileSlot] = constColorProfiles[profileSource];
        permutatedFrequencyProfiles[profileSlot] = constFrequencyProfiles[profileSource];
    }
}

/**
    @brief Sets the color of a single fixture according to the supplied color response values.

    @param &targetFixture Fixture to be adjusted.
    @param *audioAmplitudes 7 element uint32_t array of amplitudes per frequency band.
    @param colorResponse [0..0xFFFFFF] hex value that represents the color to be displayed by this fixture.
*/
void setFixtureColor(DMXFixture &targetFixture, int *audioAmplitudes, uint32_t colorResponse)
{
    // what else goes here?
    // -> setting white, strobe and so on depending on lever states, conversion hex->rgb

    // convert colors to rgb and send to fixture
    targetFixture.setRGB(colorResponse >> 16, (colorResponse & 0x00FF00) >> 8, colorResponse & 0x0000FF);
}

/**
    @brief Sets the brightness of a single fixture according to the supplied audio response values.

    @param targetFixture Fixture to be adjusted.
    @param audioAmplitudes 7 element uint32_t array of amplitudes per frequency band.
    @param audioResponse [0..0xFFFFFFF] hex value that represents the frequencies this fixture should respond to.
*/
void setFixtureBrightness(DMXFixture &targetFixture, int *audioAmplitudes, uint32_t audioResponse)
{
    uint8_t brightness = 0;
    for (uint8_t band = 0; band < 7; band++)
    {
        if (((audioResponse & ((uint32_t)0xF << (band * 4))) >> (band * 4)) == 0xF) // TODO allow this to differnetiate between the 16 possible values for each response
        {
            brightness = max(audioAmplitudes[band], brightness);
        }
    }
    targetFixture.setRGBDimmer(brightness);
}
