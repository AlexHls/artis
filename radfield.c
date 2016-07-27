#include <math.h>
#include <stdbool.h>
#include <gsl/gsl_integration.h>
#include <gsl/gsl_sf_debye.h>
#include <gsl/gsl_roots.h>
#include "grid_init.h"
#include "radfield.h"
#include "sn3d.h"

extern inline double radfield2(double nu, double T, double W);

//#const int RADFIELDBINCOUNT = 1000;
#define RADFIELDBINCOUNT 16

#define nu_lower_first_initial (CLIGHT / (15000e-8))
#define nu_upper_last_initial (CLIGHT / (1500e-8))

static double nu_lower_first = nu_lower_first_initial;

static double J_normfactor[MMODELGRID+1];

static bool radfield_initialized = false;

static const double T_R_min = MINTEMP;
static const double T_R_max = MAXTEMP * 2;

typedef enum
{
  FIT_DILUTED_BLACKBODY = 0,
  FIT_CONSTANT = 1,
} enum_bin_fit_type;

struct radfieldbin
{
  double nu_upper;   //lower wavelength boundary of this bin
  double J_raw;      //value needs to be multipled by J_normfactor
                     //to get the true value
  double nuJ_raw;
  double prev_J_normed;
  double prev_nuJ_normed;
  double prev_contribcount;
  double W;          // scaling factor
  double T_R;        // radiation temperature
  int contribcount;
  enum_bin_fit_type fit_type;
};

//static struct radfieldbin *restrict radfieldbins[MMODELGRID+1]; //heap allocated alterative
static struct radfieldbin radfieldbins[MMODELGRID+1][RADFIELDBINCOUNT];

typedef enum
{
  ONE = 0,
  TIMES_NU = 1,
} enum_prefactor;

typedef struct
{
  double T_R;
  enum_prefactor prefactor;
} gsl_planck_integral_paras;

typedef struct
{
  int modelgridindex;
  int binindex;
} gsl_T_R_solver_paras;

static FILE *restrict radfieldfile = NULL;


void radfield_init(void)
{
  if (USE_MULTIBIN_RADFIELD_MODEL)
  {
    if (radfield_initialized == false)
    {
      const char filename[100] = "radfield.out";
      radfieldfile = fopen(filename, "w");
      if (radfieldfile == NULL)
      {
        printout("Cannot open %s.\n",filename);
        exit(0);
      }
      fprintf(radfieldfile,"%8s %15s %8s %11s %11s %9s %9s %9s %9s %9s %11s\n",
              "timestep","modelgridindex","bin_num","nu_lower","nu_upper",
              "nuJ","J","J_nu_avg","ncontrib","T_R","W");
      fflush(radfieldfile);

      for (int modelgridindex = 0; modelgridindex < MMODELGRID; modelgridindex++)
      {
        //radfieldbins[modelgridindex] = (struct radfieldbin *) calloc(RADFIELDBINCOUNT, sizeof(struct radfieldbin));

        radfield_set_J_normfactor(modelgridindex, -1.0);

        double prev_nu_upper = nu_lower_first_initial;
        const double delta_nu = (nu_upper_last_initial - nu_lower_first_initial) / RADFIELDBINCOUNT; // upper limit if no edges are crossed
        //const double delta_lambda = ((1 / nu_lower_first_initial) - (1 / nu_upper_last_initial)) / RADFIELDBINCOUNT; // upper limit if no edges are crossed

        for (int binindex = 0; binindex < RADFIELDBINCOUNT; binindex++)
        {
          //const double delta_nu = pow(prev_nu_upper,2) * delta_lambda; // equally spaced in wavelength
          radfieldbins[modelgridindex][binindex].nu_upper = nu_lower_first_initial + (binindex + 1) * delta_nu;

          // Align the bin edges with bound-free edges
          if (binindex < RADFIELDBINCOUNT - 1)
          {
            for (int i = 0; i < nbfcontinua_ground; i++)
            {
              double nu_edge = phixslist[tid].groundcont[i].nu_edge;
              //printout("bf edge at %g, nu_lower_first %g, nu_upper_last %g\n",nu_edge,nu_lower_first,nu_upper_last);
              if (binindex == 0 && ((nu_edge < nu_lower_first) || (nu_edge > nu_upper_last_initial)))
                printout("MISSED bf edge at %g, nu_lower_first %g, nu_upper_last %g\n",nu_edge,nu_lower_first_initial,nu_upper_last_initial);

              if ((nu_edge > prev_nu_upper) &&  (nu_edge < radfieldbins[modelgridindex][binindex].nu_upper))
                radfieldbins[modelgridindex][binindex].nu_upper = nu_edge;
            }
          }

          radfieldbins[modelgridindex][binindex].prev_J_normed = -1.;
          radfieldbins[modelgridindex][binindex].prev_nuJ_normed = -1.;
          radfieldbins[modelgridindex][binindex].prev_contribcount = 0;
          radfieldbins[modelgridindex][binindex].J_raw = 0.;
          radfieldbins[modelgridindex][binindex].nuJ_raw = 0.;
          radfieldbins[modelgridindex][binindex].prev_contribcount = 0;
          radfieldbins[modelgridindex][binindex].W = -1.;
          radfieldbins[modelgridindex][binindex].T_R = -1.;
          //radfieldbins[modelgridindex][binindex].fit_type = FIT_CONSTANT;
          radfieldbins[modelgridindex][binindex].fit_type = FIT_DILUTED_BLACKBODY;

          prev_nu_upper = radfieldbins[modelgridindex][binindex].nu_upper;
        }
      }
      radfield_initialized = true;
    }
  }
}


static double radfield_get_bin_J(int modelgridindex, int binindex, bool averaged)
// get the normalised J_nu, from the current or current and previous timestep (averaged)
{
  if (J_normfactor[modelgridindex] <= 0.0)
  {
    printout("radfield: Fatal error: radfield_get_bin_J called before J_normfactor set for modelgridindex %d",modelgridindex);
    abort();
  }
  const double J_current = radfieldbins[modelgridindex][binindex].J_raw * J_normfactor[modelgridindex];
  if (!averaged || radfieldbins[modelgridindex][binindex].prev_J_normed < 0.)
    return J_current;
  else
    return (J_current + radfieldbins[modelgridindex][binindex].prev_J_normed) / 2;
}


static double radfield_get_bin_nuJ(int modelgridindex, int binindex, bool averaged)
{
  if (J_normfactor[modelgridindex] <= 0.0)
  {
    printout("radfield: Fatal error: radfield_get_bin_nuJ called before J_normfactor set for modelgridindex %d",modelgridindex);
    abort();
  }
  const double nuJ_current = radfieldbins[modelgridindex][binindex].nuJ_raw * J_normfactor[modelgridindex];
  if (!averaged || radfieldbins[modelgridindex][binindex].prev_nuJ_normed < 0.)
    return nuJ_current;
  else
    return (nuJ_current + radfieldbins[modelgridindex][binindex].prev_nuJ_normed) / 2;
}


static inline
double radfield_get_bin_nu_bar(int modelgridindex, int binindex)
// importantly, this is average beween the current and previous timestep
{
  //const double nuJ_bin_raw = radfieldbins[modelgridindex][binindex].nuJ_raw;
  //const double J_bin_raw = radfieldbins[modelgridindex][binindex].J_raw;
  //return nuJ_bin_raw / J_bin_raw;
  const double nuJ_sum = radfield_get_bin_nuJ(modelgridindex, binindex, true);
  const double J_sum = radfield_get_bin_J(modelgridindex, binindex, true);
  return nuJ_sum / J_sum;
}


static inline
double radfield_get_bin_nu_lower(int modelgridindex, int binindex)
{
  if (binindex > 0)
    return radfieldbins[modelgridindex][binindex - 1].nu_upper;
  else
    return nu_lower_first;
}


static inline
double radfield_get_bin_nu_upper(int modelgridindex, int binindex)
{
  return radfieldbins[modelgridindex][binindex].nu_upper;
}


static inline
int radfield_get_bin_contribcount(int modelgridindex, int binindex, bool combined)
// combined with the previous timestep
{
  const int contribcount = radfieldbins[modelgridindex][binindex].contribcount;
  if (!combined)
    return contribcount;
  else
    return contribcount + radfieldbins[modelgridindex][binindex].prev_contribcount;
}


static inline
double radfield_get_bin_W(int modelgridindex, int binindex)
{
  return radfieldbins[modelgridindex][binindex].W;
}


static inline
double radfield_get_bin_T_R(int modelgridindex, int binindex)
{
  return radfieldbins[modelgridindex][binindex].T_R;
}


static inline
int radfield_select_bin(int modelgridindex, double nu)
{
  // the slow way to search a list, one by one until found
  /*
  for (int dbinindex = 0; dbinindex < RADFIELDBINCOUNT; dbinindex++)
  {
    if (radfield_get_bin_nu_lower(modelgridindex,dbinindex) < nu &&
        radfieldbins[modelgridindex][dbinindex].nu_upper >= nu)
    {
      return dbinindex;
    }
  }
  return -1;
  */

  /*// remember the last bin number and move from there (assume function calls often have similar nu)
  while (true)
  {
    if (radfieldbins[modelgridindex][last_selected_binindex].nu_upper <= nu)
    {
      last_selected_binindex++;
      if (last_selected_binindex >= RADFIELDBINCOUNT)
      {
        last_selected_binindex = RADFIELDBINCOUNT - 1;
        return -1;
      }
    }
    else if (radfield_get_bin_nu_lower(modelgridindex, last_selected_binindex) > nu)
    {
      last_selected_binindex--;
      if (last_selected_binindex < 0)
      {
        last_selected_binindex = 0;
        return -1;
      }
    }
    else
    {
      return last_selected_binindex;
    }
  }*/

  // binary search
  int low = 0;
  int high = RADFIELDBINCOUNT - 1;
  while (low <= high)
  {
    int mid = low + ((high - low) / 2);
    if (radfieldbins[modelgridindex][mid].nu_upper <= nu)
    {
      low = mid + 1;
    }
    else if (radfield_get_bin_nu_lower(modelgridindex, mid) > nu)
    {
      high = mid - 1;
    }
    else
    {
      return mid;
    }
   }
   return -1;
}


void radfield_write_to_file(int modelgridindex, int timestep)
{
  if (USE_MULTIBIN_RADFIELD_MODEL)
  {
# ifdef _OPENMP
# pragma omp critical (radfield_out_file)
  {
# endif
    if (!radfield_initialized)
    {
      printout("Call to write_to_radfield_file before init_radfield");
      abort();
    }

    int totalcontribs = 0;
    for (int binindex = 0; binindex < RADFIELDBINCOUNT; binindex++)
      totalcontribs += radfield_get_bin_contribcount(modelgridindex, binindex, false);

    for (int binindex = -1; binindex < RADFIELDBINCOUNT; binindex++)
    {
      double nu_lower = 0.0;
      double nu_upper = 0.0;
      double nuJ_out = 0.0;
      double J_out = 0.0;
      double T_R = 0.0;
      double W = 0.0;
      double J_nu_bar = 0.0;
      int contribcount = 0;

      if (binindex >= 0)
      {
        nu_lower = radfield_get_bin_nu_lower(modelgridindex, binindex);
        nu_upper = radfieldbins[modelgridindex][binindex].nu_upper;
        nuJ_out = radfield_get_bin_nuJ(modelgridindex, binindex, true);
        J_out = radfield_get_bin_J(modelgridindex, binindex, true);
        T_R = radfield_get_bin_T_R(modelgridindex, binindex);
        W = radfield_get_bin_W(modelgridindex, binindex);
        J_nu_bar = J_out / (nu_upper - nu_lower);
        contribcount = radfield_get_bin_contribcount(modelgridindex, binindex, true);
      }
      else
      {
        nuJ_out = nuJ[modelgridindex];
        J_out = J[modelgridindex];
        T_R = get_TR(modelgridindex);
        W = get_W(modelgridindex);
        contribcount = totalcontribs;
      }

      fprintf(radfieldfile,"%8d %15d %8d %11.5e %11.5e %9.3e %9.3e %9.3e %9d %9.1f %11.5e\n",
              timestep,modelgridindex,binindex,nu_lower,nu_upper,nuJ_out,J_out,J_nu_bar,contribcount,T_R,W);
    }
    fflush(radfieldfile);
# ifdef _OPENMP
  }
# endif
  }
}


void radfield_close_file(void)
{
  if (USE_MULTIBIN_RADFIELD_MODEL)
  {
    fclose(radfieldfile);

    // for (int dmgi = 0; dmgi < MMODELGRID; dmgi++)
    //   free(radfieldbins[dmgi]);

    //free(radfieldbins);
  }
}


void radfield_zero_estimators(int modelgridindex)
// set up the new bins and clear the estimators in preparation
// for a timestep
{
  nuJ[modelgridindex] = 0.;

  if (USE_MULTIBIN_RADFIELD_MODEL)
  {
    if (!radfield_initialized)
      radfield_init();

    printout("radfield: zeroing estimators in %d bins in cell %d...",RADFIELDBINCOUNT,modelgridindex);

    for (int binindex = 0; binindex < RADFIELDBINCOUNT; binindex++)
    {
      if (J_normfactor[modelgridindex] >= 0.)
      {
        radfieldbins[modelgridindex][binindex].prev_J_normed = radfield_get_bin_J(modelgridindex, binindex, false);
        radfieldbins[modelgridindex][binindex].prev_nuJ_normed = radfield_get_bin_nuJ(modelgridindex, binindex, false);
        radfieldbins[modelgridindex][binindex].prev_contribcount = radfield_get_bin_contribcount(modelgridindex, binindex, true);
      }
      radfieldbins[modelgridindex][binindex].J_raw = 0.0;
      radfieldbins[modelgridindex][binindex].nuJ_raw = 0.0;
      radfieldbins[modelgridindex][binindex].contribcount = 0;
    }
    radfield_set_J_normfactor(modelgridindex, -1.0);
    printout("done.\n");
  }
}


inline
void radfield_update_estimators(int modelgridindex, double distance_e_cmf, double nu_cmf)
{
  int binindex = 0;
  /*if (nu_cmf <= radfield_get_bin_nu_lower(modelgridindex,binindex))
  {
    #ifdef DEBUG_ON
    printout("radfield: Extending nu_lower_first from %g down to %g\n",nu_lower_first,nu_cmf);
    #endif
    nu_lower_first = nu_cmf;
  }
  else if (nu_cmf > radfieldbins[modelgridindex][RADFIELDBINCOUNT - 1].nu_upper)
  {
    binindex = RADFIELDBINCOUNT - 1;
    #ifdef DEBUG_ON
    printout("radfield: Extending nu_upper_last from %g up to %g\n",radfield_get_bin_nu_upper(modelgridindex,binindex),nu_cmf);
    #endif
    radfieldbins[modelgridindex][binindex].nu_upper = nu_cmf;
  }
  else*/
  {
    binindex = radfield_select_bin(modelgridindex,nu_cmf);
  }

  if (binindex >= 0)
  {
    {
      #ifdef _OPENMP
      #pragma omp atomic
      #endif
      radfieldbins[modelgridindex][binindex].J_raw += distance_e_cmf;
      #ifdef _OPENMP
      #pragma omp atomic
      #endif
      radfieldbins[modelgridindex][binindex].nuJ_raw += distance_e_cmf * nu_cmf;
      #ifdef _OPENMP
      #pragma omp atomic
      #endif
      radfieldbins[modelgridindex][binindex].contribcount += 1;
    }
  }
  else
  {
    // dropping the contribution of this packet
    // printout("WARNING: radfield_update_estimators dropping packet contribution for nu_cmf %g\n",
    //          nu_cmf);
    // printout("           modelgridindex %d binindex %d nu_lower_first %g nu_upper_last %g \n",
    //          modelgridindex, binindex, nu_lower_first, radfield_get_bin_nu_upper(modelgridindex,RADFIELDBINCOUNT - 1));
  }
}


double radfield(double nu, int modelgridindex)
// mean intensity J_nu
{
  if (USE_MULTIBIN_RADFIELD_MODEL && (nts_global > FIRST_NLTE_RADFIELD_TIMESTEP-1)) // && radfieldbins[modelgridindex] != NULL
  {
    int binindex = radfield_select_bin(modelgridindex,nu);
    if (binindex >= 0)
    {
      const struct radfieldbin *restrict const bin = &radfieldbins[modelgridindex][binindex];
      if (bin->W >= 0.)
      {
        if (bin->fit_type == FIT_DILUTED_BLACKBODY)
        {
          if (bin->T_R >= 0.)
          {
            const double J_nu = radfield2(nu, bin->T_R, bin->W);
            /*if (fabs(J_nu / J_nu_fullspec - 1.0) > 0.5)
            {
              printout("WARNING: radfield: significant discrepancy. J_nu_fullspec %g, J_nu %g, nu %g bin->W %g bin->T_R %g\n",
                       J_nu_fullspec, J_nu, nu, bin->W, bin->T_R);
            }*/
            return J_nu;
          }
          else
          {
          //  return 0.;
          //  printout("WARNING: Radfield modelgridindex %d binindex %d has W %g T_R=%g<=0, using W %g T_R %g nu %g\n",
          //           modelgridindex, binindex, bin->W, bin->T_R, W_fullspec, T_R_fullspec, nu);
          }
        }
        else
        {
          return bin->W;
        }
      }
      else //W < 0
      {
        //printout("WARNING: Radfield modelgridindex %d binindex %d has W_bin=%g<0, using W %g T_R %g nu %g\n",
        //         modelgridindex, binindex, W_bin, W_fullspec, T_R_fullspec, nu);
      }
    }
    else //binindex < 0
    {
      //printout("WARNING: Radfield modelgridindex %d binindex %d nu %g nu_lower_first %g nu_upper_last %g \n",
      //         modelgridindex, binindex, nu, nu_lower_first, nu_upper_last);
    }
  }
  /*else
  {
    printout("radfield: WARNING: Radfield called before initialized. Using global T_R %g W %g nu %g modelgridindex %d\n",
             W_fullspec, T_R_fullspec, nu, modelgridindex);
  }*/

  const double T_R_fullspec = get_TR(modelgridindex);
  const double W_fullspec   = get_W(modelgridindex);
  const double J_nu_fullspec = radfield2(nu, T_R_fullspec, W_fullspec);
  return J_nu_fullspec;
}


static double gsl_integrand_planck(double nu, void *restrict paras)
{
  double T_R = ((gsl_planck_integral_paras *) paras)->T_R;
  enum_prefactor prefactor = ((gsl_planck_integral_paras *) paras)->prefactor;

  double integrand = TWOHOVERCLIGHTSQUARED * pow(nu,3) / (expm1(HOVERKB * nu / T_R));

  if (prefactor == TIMES_NU)
    integrand *= nu;

  return integrand;
}


static double planck_integral(double T_R, double nu_lower, double nu_upper, enum_prefactor prefactor)
{
  double integral = 0.;

  double error = 0.;
  const double epsrel = 1e-10;
  const double epsabs = 0.;

  gsl_integration_workspace *w = gsl_integration_workspace_alloc(65536);

  gsl_planck_integral_paras intparas;
  intparas.T_R = T_R;
  intparas.prefactor = prefactor;

  gsl_function F_planck;
  F_planck.function = &gsl_integrand_planck;
  F_planck.params = &intparas;

  gsl_error_handler_t *gsl_error_handler = gsl_set_error_handler_off();
  int status = gsl_integration_qag(&F_planck, nu_lower, nu_upper, epsabs, epsrel, 65536, GSL_INTEG_GAUSS61, w, &integral, &error);
  if (status != 0)
  {
    printout("planck_integral integrator status %d, GSL_FAILURE= %d. Integral value %g, setting to zero.\n", status,GSL_FAILURE,integral);
    integral = 0.;
  }
  gsl_set_error_handler(gsl_error_handler);

  gsl_integration_workspace_free(w);

  return integral;
}


static double planck_integral_analytic(double T_R, double nu_lower, double nu_upper, enum_prefactor prefactor)
{
  double integral = 0.;

  if (prefactor == TIMES_NU)
  {
    double debye_upper = gsl_sf_debye_4(HOVERKB * nu_upper / T_R) * pow(nu_upper,4);
    double debye_lower = gsl_sf_debye_4(HOVERKB * nu_lower / T_R) * pow(nu_lower,4);
    integral = TWOHOVERCLIGHTSQUARED * (debye_upper - debye_lower) * T_R / HOVERKB / 4;
  }
  else
  {
    double debye_upper = gsl_sf_debye_3(HOVERKB * nu_upper / T_R) * pow(nu_upper,3);
    double debye_lower = gsl_sf_debye_3(HOVERKB * nu_lower / T_R) * pow(nu_lower,3);
    integral = TWOHOVERCLIGHTSQUARED * (debye_upper - debye_lower) * T_R / HOVERKB / 3;

    if (integral == 0.)
    {
      /*double upperexp = exp(HOVERKB * nu_upper / T_R);
      double upperint = - pow(nu_upper,4) / 4
                        + pow(nu_upper,3) * log(1 - upperexp) / HOVERKB
                        + 3 * pow(nu_upper,2) * polylog(2,upperexp) / pow(HOVERKB,2)
                        - 6 * nu_upper * polylog(3,upperexp) / pow(HOVERKB,3)
                        + 6 * polylog(4,upperexp) / pow(HOVERKB,4);
      double lowerexp = exp(HOVERKB * nu_lower / T_R);
      double lowerint = - pow(nu_lower,4) / 4
                        + pow(nu_lower,3) * log(1 - lowerexp) / HOVERKB
                        + 3 * pow(nu_lower,2) * polylog(2,lowerexp) / pow(HOVERKB,2)
                        - 6 * nu_lower * polylog(3,lowerexp) / pow(HOVERKB,3)
                        + 6 * polylog(4,lowerexp) / pow(HOVERKB,4);
      double integral2 = TWOHOVERCLIGHTSQUARED * (upperint - lowerint);

      printout("planck_integral_analytic is zero. debye_upper %g debye_lower %g. Test alternative %g\n",
               debye_upper,debye_lower,integral2);*/
    }
  }

  return integral;
}


static double delta_nu_bar(double T_R, void *restrict paras)
// difference between the average nu and the average nu of a planck function
// at temperature T_R, in the frequency range corresponding to a bin
{
  int modelgridindex = ((gsl_T_R_solver_paras *) paras)->modelgridindex;
  int binindex = ((gsl_T_R_solver_paras *) paras)->binindex;

  double nu_lower = radfield_get_bin_nu_lower(modelgridindex, binindex);
  double nu_upper = radfieldbins[modelgridindex][binindex].nu_upper;

  double nu_bar_estimator = radfield_get_bin_nu_bar(modelgridindex, binindex);

  double nu_times_planck_numerical = planck_integral(T_R, nu_lower, nu_upper, TIMES_NU);
  double planck_integral_numerical = planck_integral(T_R, nu_lower, nu_upper, ONE);
  double nu_bar_planck_T_R = nu_times_planck_numerical / planck_integral_numerical;

  /*double nu_times_planck_integral = planck_integral_analytic(T_R, nu_lower, nu_upper, TIMES_NU);
  double planck_integral_result = planck_integral_analytic(T_R, nu_lower, nu_upper, ONE);
  double nu_bar_planck = nu_times_planck_integral / planck_integral_result;

  //printout("nu_bar %g nu_bar_planck(T=%g) %g\n",nu_bar,T_R,nu_bar_planck);

  if (!isfinite(nu_bar_planck))
  {
    double nu_times_planck_numerical = planck_integral(T_R, nu_lower, nu_upper, TIMES_NU);
    double planck_integral_numerical = planck_integral(T_R, nu_lower, nu_upper, ONE);
    double nu_bar_planck_numerical = nu_times_planck_numerical / planck_integral_numerical;

    printout("planck_integral_analytic is %g. Replacing with numerical result of %g.\n",nu_bar_planck,nu_bar_planck_numerical);
    nu_bar_planck = nu_bar_planck_numerical;
  }*/

  double delta_nu_bar = nu_bar_planck_T_R - nu_bar_estimator;
  //double delta_nu_bar = nu_bar_planck_T_R / nu_bar_estimator - 1.0;

  //printout("delta_nu_bar %g nu_bar_planck %g\n",delta_nu_bar,nu_bar_planck);

  return delta_nu_bar;
}


static double find_T_R(int modelgridindex, int binindex)
{
  double T_R = 0.0;

  gsl_T_R_solver_paras paras;
  paras.modelgridindex = modelgridindex;
  paras.binindex = binindex;

  /// Check whether the equation has a root in [T_min,T_max]
  double delta_nu_bar_min = delta_nu_bar(T_R_min,&paras);
  double delta_nu_bar_max = delta_nu_bar(T_R_max,&paras);

  printout("find_T_R: bin %4d delta_nu_bar(T_R_min) %g, delta_nu_bar(T_R_max) %g\n",
           binindex, delta_nu_bar_min,delta_nu_bar_max);

  if (!isfinite(delta_nu_bar_min) || !isfinite(delta_nu_bar_max))
    delta_nu_bar_max = delta_nu_bar_min = -1;

  if (delta_nu_bar_min * delta_nu_bar_max < 0)
  {
    /// If there is a root in the interval, solve for T_R

    const double epsrel = 1e-4;
    const double epsabs = 0.;
    const int maxit = 100;

    gsl_function find_T_R_f;
    find_T_R_f.function = &delta_nu_bar;
    find_T_R_f.params = &paras;

    ///one dimensional gsl root solver, bracketing type
    gsl_root_fsolver *T_R_solver = gsl_root_fsolver_alloc(gsl_root_fsolver_brent);
    gsl_root_fsolver_set(T_R_solver, &find_T_R_f, T_R_min, T_R_max);
    int iteration_num = 0;
    int status = GSL_CONTINUE;
    do
    {
      iteration_num++;
      status = gsl_root_fsolver_iterate(T_R_solver);
      T_R = gsl_root_fsolver_root(T_R_solver);
      //cell[cellnumber].T_e = T_e;

      double T_R_lower = gsl_root_fsolver_x_lower(T_R_solver);
      double T_R_upper = gsl_root_fsolver_x_upper(T_R_solver);
      status = gsl_root_test_interval(T_R_lower,T_R_upper,epsabs,epsrel);

      //printout("find_T_R: bin %4d iter %d, T_R is between %7.1f and %7.1f, guess %7.1f, delta_nu_bar %g, status %d\n",
      //         binindex,iteration_num,T_R_lower,T_R_upper,T_R,delta_nu_bar(T_R,&paras),status);
    }
    while (status == GSL_CONTINUE && iteration_num < maxit);

    if (status == GSL_CONTINUE)
      printout(
        "[warning] find_T_R: T_R did not converge within %d iterations\n", maxit);

    gsl_root_fsolver_free(T_R_solver);
  }
  else if (delta_nu_bar_max < 0)
  {
    /// Thermal balance equation always negative ===> T_R = T_min
    /// Calculate the rates again at this T_e to print them to file
    T_R = T_R_max;
    printout("find_T_R: bin %4d no solution in interval, clamping to T_R_max=%g\n",
             binindex,T_R_max);
  }
  else
  {
    T_R = T_R_min;
    printout("find_T_R: bin %4d no solution in interval, clamping to T_R_min=%g\n",
             binindex,T_R_min);
  }

  return T_R;
}


void radfield_fit_parameters(int modelgridindex, int timestep)
// finds the best fitting W and temperature parameters in each spectral bin
// using J and nuJ
{
  double T_J, T_R, W;
  get_radfield_params_fullspec(J[modelgridindex],nuJ[modelgridindex],modelgridindex,&T_J,&T_R,&W);
  set_TJ(modelgridindex,T_J);
  set_TR(modelgridindex,T_R);
  set_W(modelgridindex,W);

  if (USE_MULTIBIN_RADFIELD_MODEL)
  {
    if (J_normfactor[modelgridindex] <= 0)
    {
      printout("radfield: FATAL J_normfactor = %g in cell %d at call to radfield_fit_parameters",J_normfactor[modelgridindex],modelgridindex);
      abort();
    }

    const double T_R_fullspec = get_TR(modelgridindex);
    const double J_fullspec = J[modelgridindex];
    //double planck_integral_zero_inf = STEBO * pow(T_R_fullspec,4) / PI;

    printout("Full-spectrum fit radfield params for cell %d: J %g, T_R %g, W %g\n",
             modelgridindex, J_fullspec, T_R_fullspec, get_W(modelgridindex));

    double J_bin_sum = 0.;
    for (int binindex = 0; binindex < RADFIELDBINCOUNT; binindex++)
      J_bin_sum += radfield_get_bin_J(modelgridindex,binindex,true);

    printout("radfield bins sum to J of %g out of total J %g (%.1f%%).\n",
             J_bin_sum,J[modelgridindex],100.*J_bin_sum/J[modelgridindex]);
    printout("radfield: Finding parameters for %d bins...\n",RADFIELDBINCOUNT);

    for (int binindex = 0; binindex < RADFIELDBINCOUNT; binindex++)
    {
      double J_bin = radfield_get_bin_J(modelgridindex,binindex,true);
      double nu_lower = radfield_get_bin_nu_lower(modelgridindex,binindex);
      double nu_upper = radfieldbins[modelgridindex][binindex].nu_upper;
      double T_R_bin = -1.0;
      double W_bin = -1.0;
      int contribcount = radfield_get_bin_contribcount(modelgridindex,binindex,true);
      if (contribcount > 10)
      {
        enum_bin_fit_type bin_fit_type = radfieldbins[modelgridindex][binindex].fit_type;
        if (bin_fit_type == FIT_DILUTED_BLACKBODY)
        {
          T_R_bin = find_T_R(modelgridindex, binindex);

          double planck_integral_result = planck_integral(T_R_bin, nu_lower, nu_upper, ONE);

          W_bin = J_bin / planck_integral_result;

          if (W_bin > 1e4)
          {
            printout("W %g too high, try setting T_R of bin %d to %g. J_bin %g planck_integral %g\n",W_bin,binindex,T_R_max,planck_integral_result);
            planck_integral_result = planck_integral(T_R_max, nu_lower, nu_upper, ONE);
            W_bin = J_bin / planck_integral_result;
            if (W_bin > 1e4)
            {
              printout("W still very high, W=%g. Zeroing bin...\n",W_bin);
              T_R_bin = -99.0;
              W_bin = 0.;
            }
            else
            {
              printout("new W is %g. Much better :)\n",W_bin);
            }
          }
        }
        else if (bin_fit_type == FIT_CONSTANT)
        {
          T_R_bin = -1.;
          W_bin = J_bin / (nu_upper - nu_lower);
        }
        else
        {
          printout("radfield_fit_parameters: unknown fit type %d for bin %d\n",bin_fit_type,binindex);
          T_R_bin = -1.;
          W_bin = -1.;
        }
      }
      else if (contribcount == 0)
      {
        T_R_bin = 0.;
        W_bin = 0.;
      }
      else
      {
        T_R_bin = -1;
        W_bin = -1;
      }
      radfieldbins[modelgridindex][binindex].T_R = T_R_bin;
      radfieldbins[modelgridindex][binindex].W = W_bin;
    }

    /*for (int binindex = 0; binindex < RADFIELDBINCOUNT; binindex++)
    {
      double J_bin = radfield_get_bin_J(modelgridindex,binindex);
      double T_R_bin = radfield_get_bin_T_R(modelgridindex,binindex);
      double W_bin = radfield_get_bin_W(modelgridindex,binindex);
      printout("bin %4d: J %g, T_R %7.1f, W %12.5e\n",
             binindex, J_bin, T_R_bin, W_bin);
    }*/
    radfield_write_to_file(modelgridindex,timestep);
  }
}


void get_radfield_params_fullspec(double J, double nuJ, int modelgridindex, double *T_J, double *T_R, double *W)
{
  double nubar = nuJ/J;
  if (!isfinite(nubar) || nubar == 0.)
  {
    /// Return old T_R
    printout("[warning] update_grid: T_R estimator infinite in cell %d, use value of last timestep\n",modelgridindex);
    *T_J = modelgrid[modelgridindex].TJ;
    *T_R = modelgrid[modelgridindex].TR;
    *W = modelgrid[modelgridindex].W;
  }
  else
  {
    *T_J = pow(PI / STEBO * J,1/4.);
    if (*T_J > MAXTEMP)
    {
      printout("[warning] update_grid: temperature estimator T_J=%g exceeds T_max=%g in cell %d. Set T_J = T_max!\n",*T_J,MAXTEMP,modelgridindex);
      *T_J = MAXTEMP;
    }
    if (*T_J < MINTEMP)
    {
      printout("[warning] update_grid: temperature estimator T_J=%g below T_min %g in cell %d. Set T_J = T_min!\n",*T_J,MINTEMP,modelgridindex);
      *T_J = MINTEMP;
    }

    *T_R = H * nubar / KB / 3.832229494;
    if (*T_R > MAXTEMP)
    {
      printout("[warning] update_grid: temperature estimator T_R=%g exceeds T_max=%g in cell %d. Set T_R = T_max!\n",*T_R,MAXTEMP,modelgridindex);
      *T_R = MAXTEMP;
    }
    if (*T_R < MINTEMP)
    {
      printout("[warning] update_grid: temperature estimator T_R=%g below T_min %g in cell %d. Set T_R = T_min!\n",*T_R,MINTEMP,modelgridindex);
      *T_R = MINTEMP;
    }

    *W = PI * J / STEBO / pow(*T_R,4);
  }
}


void radfield_set_J_normfactor(int modelgridindex, double normfactor)
{
  J_normfactor[modelgridindex] = normfactor;
}


void radfield_reduce_estimators(int my_rank)
{
  #ifdef MPI_ON
  for (int modelgridindex = 0; modelgridindex < MMODELGRID; modelgridindex++)
  {
    for (int binindex = 0; binindex < RADFIELDBINCOUNT; binindex++)
    {
      double J_raw_reduced = 0.;
      MPI_Barrier(MPI_COMM_WORLD);
      MPI_Reduce(&radfieldbins[modelgridindex][binindex].J_raw, &J_raw_reduced, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
      double nuJ_raw_reduced = 0.;
      MPI_Barrier(MPI_COMM_WORLD);
      MPI_Reduce(&radfieldbins[modelgridindex][binindex].nuJ_raw, &nuJ_raw_reduced, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
      int contribcount_reduced = 0;
      MPI_Barrier(MPI_COMM_WORLD);
      printout("MPI: Pre-reduction, process %d binindex %d has a individual contribcount of %d\n",my_rank,binindex,radfieldbins[modelgridindex][binindex].contribcount);
      MPI_Reduce(&radfieldbins[modelgridindex][binindex].contribcount, &contribcount_reduced, 1, MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD);
      if (my_rank == 0)
      {
        radfieldbins[modelgridindex][binindex].J_raw = J_raw_reduced;
        radfieldbins[modelgridindex][binindex].nuJ_raw = nuJ_raw_reduced;
        radfieldbins[modelgridindex][binindex].contribcount = contribcount_reduced;
        printout("MPI: Process %d binindex %d took the reduced contribcount of %d\n",my_rank,binindex,radfieldbins[modelgridindex][binindex].contribcount);
      }
    }
  }
  #endif
}

void radfield_broadcast_estimators(int my_rank)
{
  #ifdef MPI_ON
  for (int modelgridindex = 0; modelgridindex < MMODELGRID; modelgridindex++)
  {
    for (int binindex = 0; binindex < RADFIELDBINCOUNT; binindex++)
    {
      MPI_Bcast(&radfieldbins[modelgridindex][binindex].J_raw, 1, MPI_DOUBLE, 0, MPI_COMM_WORLD);
      MPI_Bcast(&radfieldbins[modelgridindex][binindex].nuJ_raw, 1, MPI_DOUBLE, 0, MPI_COMM_WORLD);
      MPI_Bcast(&radfieldbins[modelgridindex][binindex].contribcount, 1, MPI_INT, 0, MPI_COMM_WORLD);
      printout("MPI: After broadcast: Process %d binindex %d has a contribcount of %d\n",my_rank,binindex,radfieldbins[modelgridindex][binindex].contribcount);
    }
  }
  #endif
}
