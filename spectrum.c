#include "sn3d.h"
#include "exspec.h"
#include "atomic.h"
#include "spectrum.h"
#include "vectors.h"

/*int make_spectrum()
/// Routine to make a MC spectrum from the packets.
{
  gather_spectrum(0);
  write_spectrum();

  return 0;
}*/

#define traceemissionregion

#ifdef traceemissionregion
  #define traceemiss_nulower (CLIGHT / (6000e-8))  // in Angstroms
  #define traceemiss_nuupper (CLIGHT / (5600e-8))  // in Angstroms
  #define traceemiss_timestepmin 70
  #define traceemiss_timestepmax 90

  typedef struct emissioncontrib {
    double fluxcontrib;
    int lineindex;
  } emissioncontrib;

  struct emissioncontrib *traceemisscontributions;
  double traceemiss_totalflux = 0.;
  static int compare_emisscontrib(const void *p1, const void *p2)
  {
      const struct emissioncontrib *elem1 = p1;
      const struct emissioncontrib *elem2 = p2;

     if (elem1->fluxcontrib < elem2->fluxcontrib)
        return 1;
     else if (elem1->fluxcontrib > elem2->fluxcontrib)
        return -1;
     else
        return 0;
  }
#endif



void write_spectrum(FILE *spec_file, FILE *emission_file, FILE *absorption_file)
{
  //FILE *spec_file,*emission_file;

  /*
  float dum1, dum2;
  /// The spectra are now done - just need to print them out.
  if (file_set)
  {
    if ((spec_file = fopen("spec.out", "r")) == NULL)
    {
      printout("Cannot open spec_file.txt.\n");
      exit(0);
    }
    fscanf(spec_file, "%g ", &dum1);

    for (p = 0; p < ntbins; p++)
    {
      fscanf(spec_file, "%g ", &dum1);
    }

    for (m=0; m < nnubins; m++)
    {
      fscanf(spec_file, "%g ", &dum1);

      for (p = 0; p < ntbins; p++)
      {
        fscanf(spec_file, "%g ",
                &dum2);
        spectra[p].flux[m] += dum2;
      }
    }
    fclose(spec_file);
  }


  if ((emission_file = fopen("emission.out", "w")) == NULL)
  {
    printf("Cannot open emission file\n");
    exit(0);
  }
  /// write to file
  if ((spec_file = fopen("spec.out", "w+")) == NULL){
    printout("Cannot open spec_file.txt.\n");
    exit(0);
  }
  */

  fprintf(spec_file, "%g ", 0.0);
  for (int p = 0; p < ntbins; p++)
  {
    /// ????????????????????????????????????????????????????????????????????????????????????????????????
    /// WHY HERE OTHER CALCULATION OF "SPECTRA.MIDTIME" THAN FOR time_step.mid ?????????????????????????
    fprintf(spec_file, "%g ", (spectra[p].lower_time + (spectra[p].delta_t/2))/DAY);
  }
  fprintf(spec_file, "\n");

  for (int m = 0; m < nnubins; m++)
  {
    fprintf(spec_file, "%g ", ((spectra[0].lower_freq[m]+(spectra[0].delta_freq[m]/2))));

    for (int p = 0; p < ntbins; p++)
    {
      fprintf(spec_file, "%g ", spectra[p].flux[m]);
      if (do_emission_res == 1)
      {
        for (int i = 0; i < 2*nelements*maxion+1; i++)
          fprintf(emission_file, "%g ", spectra[p].stat[m].emission[i]);
        fprintf(emission_file, "\n");
        for (int i = 0; i < nelements*maxion; i++)
          fprintf(absorption_file, "%g ", spectra[p].stat[m].absorption[i]);
        fprintf(absorption_file, "\n");
      }
    }
    fprintf(spec_file, "\n");
  }

  /*
  fclose(spec_file);
  fclose(emission_file);
  */
}


static void add_to_spec(const EPKT *pkt_ptr)
/*Routine to add a packet to the outcoming spectrum.*/
{
  /** Need to (1) decide which time bin to put it in and (2) which frequency bin. */
  int nproc;

  /// Put this into the time grid.
  double t_arrive = pkt_ptr->arrive_time;
  if (t_arrive > tmin && t_arrive < tmax)
  {
    int nt = (log(t_arrive) - log(tmin)) / dlogt;
    if (pkt_ptr->nu_rf > nu_min_r && pkt_ptr->nu_rf < nu_max_r)
    {
      int nnu = (log(pkt_ptr->nu_rf) - log(nu_min_r)) /  dlognu;
      double deltaE = pkt_ptr->e_rf / spectra[nt].delta_t / spectra[nt].delta_freq[nnu] / 4.e12 / PI / PARSEC / PARSEC / nprocs;
      spectra[nt].flux[nnu] += deltaE;

      int et = pkt_ptr->emissiontype;
      if (et >= 0)
      {
        /// bb-emission
        const int element = linelist[et].elementindex;
        const int ion = linelist[et].ionindex;
        nproc = element*maxion+ion;
        #ifdef traceemissionregion
        if (nt >= traceemiss_timestepmin && nt <= traceemiss_timestepmax)
        {
          if (pkt_ptr->nu_rf >= traceemiss_nulower && pkt_ptr->nu_rf <= traceemiss_nuupper)
          {
            traceemisscontributions[et].fluxcontrib += deltaE;
            traceemiss_totalflux += deltaE;
            // printout("packet in range, Z=%d ion_stage %d upperlevel %4d lowerlevel %4d fluxcontrib %g linecontrib %g index %d nlines %d\n",
                    //  get_element(element), get_ionstage(element, ion), linelist[et].upperlevelindex, linelist[et].lowerlevelindex, deltaE, traceemisscontributions[et].fluxcontrib, et, nlines);
          }
        }
        #endif
      }
      else if (et == -9999999)
      {
        /// ff-emission
        nproc = 2*nelements*maxion;
      }
      else
      {
        /// bf-emission
        et = -1*et - 1;
        const int element = bflist[et].elementindex;
        const int ion = bflist[et].ionindex;
        nproc = nelements*maxion + element*maxion+ion;
      }
      spectra[nt].stat[nnu].emission[nproc] += deltaE;

      nnu = (log(pkt_ptr->absorptionfreq) - log(nu_min_r)) /  dlognu;
      if (nnu >= 0 && nnu < MNUBINS)
      {
        deltaE = pkt_ptr->e_rf / spectra[nt].delta_t / spectra[nt].delta_freq[nnu] / 4.e12 / PI / PARSEC /PARSEC / nprocs;
        const int at = pkt_ptr->absorptiontype;
        if (at >= 0)
        {
          /// bb-emission
          const int element = linelist[at].elementindex;
          const int ion = linelist[at].ionindex;
          nproc = element*maxion+ion;
          spectra[nt].stat[nnu].absorption[nproc] += deltaE;
        }
      }
    }
  }
}


static void init_spectrum(void)
{
  if (nnubins > MNUBINS)
  {
    printout("Too many frequency bins in spectrum - reducing.\n");
    nnubins = MNUBINS;
  }
  if (ntbins > MTBINS)
  {
    printout("Too many time bins in spectrum - reducing.\n");
    ntbins = MTBINS;
  }

  // start by setting up the time and frequency bins.
  // it is all done interms of a logarithmic spacing in both t and nu - get the
  // step sizes first.
  ///Should be moved to input.c or exspec.c
  dlogt = (log(tmax) - log(tmin))/ntbins;
  dlognu = (log(nu_max_r) - log(nu_min_r))/nnubins;

  for (int n = 0; n < ntbins; n++)
  {
    spectra[n].lower_time = exp( log(tmin) + (n * (dlogt)));
    spectra[n].delta_t = exp( log(tmin) + ((n+1) * (dlogt))) - spectra[n].lower_time;
    for (int m = 0; m < nnubins; m++)
    {
      spectra[n].lower_freq[m] = exp( log(nu_min_r) + (m * (dlognu)));
      spectra[n].delta_freq[m] = exp( log(nu_min_r) + ((m+1) * (dlognu))) - spectra[n].lower_freq[m];
      spectra[n].flux[m] = 0.0;
      for (int i = 0; i < 2 * nelements * maxion + 1; i++)
        spectra[n].stat[m].emission[i] = 0;  ///added
      for (int i = 0; i < nelements*maxion; i++)
        spectra[n].stat[m].absorption[i] = 0;  ///added
    }
  }
}

void gather_spectrum(int depth)
{
  /// Set up the spectrum grid and initialise the bins to zero.
  init_spectrum();

  #ifdef traceemissionregion
  traceemiss_totalflux = 0.;
  traceemisscontributions = malloc(nlines*sizeof(emissioncontrib));
  for (int i = 0; i < nlines; i++)
  {
    traceemisscontributions[i].fluxcontrib = 0.;
    traceemisscontributions[i].lineindex = i; // this will be important when the list gets sorted
  }
  #endif

  if (depth < 0)
  {
    /// Do not extract depth-dependent spectra
    /// Now add the energy of all the escaping packets to the
    /// appropriate bins.
    for (int p = 0; p < nepkts; p++)
    {
      add_to_spec(&epkts[p]);
    }
  }
  else
  {
    /// Extract depth-dependent spectra
    /// Set velocity cut
    double vcut;
    if (depth < 9)
      vcut = (depth + 1.) * vmax / 10.;
    else
      vcut = 100 * vmax;     /// Make sure that all escaping packets are taken for
                             /// depth=9 . For 2d and 3d models the corners of a
                             /// simulation box contain material with v>vmax.

    /// Now add the energy of all the escaping packets to the
    /// appropriate bins.
    for (int p = 0; p < nepkts; p++)
    {
      double vem = sqrt(pow(epkts[p].em_pos[0],2) + pow(epkts[p].em_pos[1],2) + pow(epkts[p].em_pos[2],2)) / (epkts[p].em_time);
      //printout("vem %g, vcut %g, vmax %g, time %d\n",vem,vcut,vmax,pkt_ptr->em_time);
      if (vem < vcut)
        add_to_spec(&epkts[p]);
    }
  }

  #ifdef traceemissionregion
  qsort(traceemisscontributions, nlines, sizeof(emissioncontrib), compare_emisscontrib);
  printout("Top line emission contributions in the range lambda [%5.1f, %5.1f] timestep [%d, %d] (flux %g)\n",
           1e8 * CLIGHT / traceemiss_nuupper, 1e8 * CLIGHT / traceemiss_nulower, traceemiss_timestepmin, traceemiss_timestepmax,
           traceemiss_totalflux);

  // display the top entries of the sorted list
  int nlines_limited = nlines;
  if (nlines > 50)
    nlines = 50;
  for (int i = 0; i < nlines_limited; i++)
  {
    const double fluxcontrib = traceemisscontributions[i].fluxcontrib;
    if (fluxcontrib / traceemiss_totalflux > 0.01 || i < 5) // lines that contribute at least 5% of the flux, with a minimum of 5 lines and max of 50
    {
      const int lineindex = traceemisscontributions[i].lineindex;
      const int element = linelist[lineindex].elementindex;
      const int ion = linelist[lineindex].ionindex;
      printout("flux %7.2e (%5.1f%%) Z=%d ion_stage %d upperlevel %4d lowerlevel %4d coll_str %g A %8.2e forbidden %d\n",
               fluxcontrib, 100 * fluxcontrib / traceemiss_totalflux, get_element(element),
               get_ionstage(element, ion), linelist[lineindex].upperlevelindex, linelist[lineindex].lowerlevelindex,
             linelist[lineindex].coll_str, einstein_spontaneous_emission(lineindex), linelist[lineindex].forbidden);
     }
     else
      break;
  }

  free(traceemisscontributions);
  #endif
}


static void add_to_spec_res(EPKT *pkt_ptr, int current_abin)
// Routine to add a packet to the outgoing spectrum.
{
  /* Need to (1) decide which time bin to put it in and (2) which frequency bin. */

  /* Time bin - we know that it escaped at "escape_time". However, we have to allow
     for travel time. Use the formula in Leon's paper.
     The extra distance to be travelled beyond the reference surface is ds = r_ref (1 - mu).
  */

  int nproc;

  double xhat[3] = {1.0, 0.0, 0.0};

  /// Angle resolved case: need to work out the correct angle bin
  const double costheta = dot(pkt_ptr->dir, syn_dir);
  const double thetabin = ((costheta + 1.0) * sqrt(MABINS) / 2.0);
  double vec1[3];
  cross_prod(pkt_ptr->dir, syn_dir, vec1);
  double vec2[3];
  cross_prod(xhat, syn_dir, vec2);
  const double cosphi = dot(vec1,vec2) / vec_len(vec1) / vec_len(vec2);

  double vec3[3];
  cross_prod(vec2, syn_dir, vec3);
  const double testphi = dot(vec1,vec3);

  int phibin;
  if (testphi > 0)
  {
    phibin = (acos(cosphi) / 2. / PI * sqrt(MABINS));
  }
  else
  {
    phibin = ((acos(cosphi) + PI) / 2. / PI * sqrt(MABINS));
  }
  const int na = (thetabin*sqrt(MABINS)) + phibin;

  /// Add only packets which escape to the current angle bin
  if (na == current_abin)
  {
    /// Put this into the time grid.
    const double t_arrive = pkt_ptr->arrive_time;
    if (t_arrive > tmin && t_arrive < tmax)
    {
      const int nt = (log(t_arrive) - log(tmin)) / dlogt;

      /// and the correct frequency bin.
      if (pkt_ptr->nu_rf > nu_min_r && pkt_ptr->nu_rf < nu_max_r)
      {
        int nnu = (log(pkt_ptr->nu_rf) - log(nu_min_r)) /  dlognu;
        double deltaE = pkt_ptr->e_rf / spectra[nt].delta_t / spectra[nt].delta_freq[nnu] / 4.e12 / PI / PARSEC / PARSEC * MABINS / nprocs;
        spectra[nt].flux[nnu] += deltaE;

        if (do_emission_res == 1)
        {
          int et = pkt_ptr->emissiontype;
          if (et >= 0)
          {
            /// bb-emission
            const int element = linelist[et].elementindex;
            const int ion = linelist[et].ionindex;
            nproc = element*maxion+ion;
          }
          else if (et == -9999999)
          {
            /// ff-emission
            nproc = 2*nelements*maxion;
          }
          else
          {
            /// bf-emission
            et = -1*et - 1;
            const int element = bflist[et].elementindex;
            const int ion = bflist[et].ionindex;
            nproc = nelements*maxion + element*maxion+ion;
          }
          spectra[nt].stat[nnu].emission[nproc] += deltaE;

          nnu = (log(pkt_ptr->absorptionfreq) - log(nu_min_r)) /  dlognu;
          if (nnu >= 0 && nnu < MNUBINS)
          {
            deltaE = pkt_ptr->e_rf / spectra[nt].delta_t / spectra[nt].delta_freq[nnu] / 4.e12 / PI / PARSEC /PARSEC * MABINS / nprocs;
            int at = pkt_ptr->absorptiontype;
            if (at >= 0)
            {
              /// bb-emission
              const int element = linelist[at].elementindex;
              const int ion = linelist[at].ionindex;
              nproc = element*maxion+ion;
              spectra[nt].stat[nnu].absorption[nproc] += deltaE;
            }
          }
        }
      }
    }
  }
}


void gather_spectrum_res(int current_abin)
{
  /// Set up the spectrum grid and initialise the bins to zero.
  init_spectrum();

  /// Now add the energy of all the escaping packets to the
  /// appropriate bins.
  for (int p = 0; p < nepkts; p++)
  {
    add_to_spec_res(&epkts[p], current_abin);
  }

}


