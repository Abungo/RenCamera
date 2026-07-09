/* Generated test code to dump a table of data for external validation
 * of the noise model parameters.
 */
#include <stdio.h>
#include <assert.h>
double compute_noise_model_entry_S(int plane, int sens);
double compute_noise_model_entry_O(int plane, int sens);
int main(void) {
    for (int plane = 0; plane < 4; plane++) {
        for (int sens = 72; sens <= 6400; sens += 100) {
            double o = compute_noise_model_entry_O(plane, sens);
            double s = compute_noise_model_entry_S(plane, sens);
            printf("%d,%d,%lf,%lf\n", plane, sens, o, s);
        }
    }
    return 0;
}

/* Generated functions to map a given sensitivity to the O and S noise
 * model parameters in the DNG noise model. The planes are in
 * R, Gr, Gb, B order.
 */
double compute_noise_model_entry_S(int plane, int sens) {
    static double noise_model_A[] = { 1.221010720568816e-06,1.2032388315440924e-06,1.2057633779412615e-06,1.2132386573444483e-06 };
    static double noise_model_B[] = { 3.897314940790411e-06,1.2737596748407022e-05,1.2855556632956738e-05,9.355504019513958e-06 };
    double A = noise_model_A[plane];
    double B = noise_model_B[plane];
    double s = A * sens + B;
    return s < 0.0 ? 0.0 : s;
}

double compute_noise_model_entry_O(int plane, int sens) {
    static double noise_model_C[] = { 1.9620154406927164e-12,1.0356528849976175e-12,9.306512944182513e-13,1.575566837302869e-12 };
    static double noise_model_D[] = { 3.5510901100348254e-07,1.1625249107535592e-07,8.154741532618057e-08,2.074839480975898e-07 };
    double digital_gain = (sens / 3200.0) < 1.0 ? 1.0 : (sens / 3200.0);
    double C = noise_model_C[plane];
    double D = noise_model_D[plane];
    double o = C * sens * sens + D * digital_gain * digital_gain;
    return o < 0.0 ? 0.0 : o;
}
