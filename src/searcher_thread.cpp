// Copyright 2012 Evrytania LLC (http://www.evrytania.com)
//
// Written by James Peroulas <james@evrytania.com>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <unistd.h>
#include <itpp/itbase.h>
#include <itpp/signal/transforms.h>
#include <boost/math/special_functions/gamma.hpp>
#include <boost/thread.hpp>
#include <boost/thread/condition.hpp>
#include <list>
#include <sstream>
#include <signal.h>
#include <queue>
#include "rtl-sdr.h"
#include "common.h"
#include "macros.h"
#include "lte_lib.h"
#include "constants.h"
#include "capbuf.h"
#include "itpp_ext.h"
#include "searcher.h"
#include "dsp.h"
#include "rtl-sdr.h"
#include "LTE-Tracker.h"

using namespace itpp;
using namespace std;

#define DS_COMB_ARM 2

// This is the searcher process. It requests captured data from the main
// thread and launches a new thread for every cell it finds. Each new
// cell thread then requests sample data from the main thread.
void searcher_thread(
  capbuf_sync_t & capbuf_sync,
  global_thread_data_t & global_thread_data,
  tracked_cell_list_t & tracked_cell_list
) {
  if (verbosity>=1) {
    cout << "Searcher process has been launched." << endl;
  }

  // Shortcut
  double & fc=global_thread_data.fc;

  // Loop forever.
  while (true) {
    // Request data.
    {
      boost::mutex::scoped_lock lock(capbuf_sync.mutex);
      capbuf_sync.request=true;

      // Wait for data to become ready.
      capbuf_sync.condition.wait(lock);
    }

    // Get the current frequency offset
    double k_factor;
    vec f_search_set(1);
    {
      boost::mutex::scoped_lock lock(global_thread_data.frequency_offset_mutex);
      k_factor=(fc-global_thread_data.frequency_offset)/fc;
      f_search_set(0)=global_thread_data.frequency_offset;
    }

    // Results are stored in this vector.
    list<Cell> detected_cells;

    // Local reference to the capture buffer.
    cvec &capbuf=capbuf_sync.capbuf;

    // Correlate
    mat xc_incoherent_collapsed_pow;
    imat xc_incoherent_collapsed_frq;
    vf3d xc_incoherent_single;
    vf3d xc_incoherent;
    vec sp_incoherent;
    vcf3d xc;
    vec sp;
    uint16 n_comb_xc;
    uint16 n_comb_sp;
    if (verbosity>=2) {
      cout << "  Calculating PSS correlations" << endl;
    }
    xcorr_pss(capbuf,f_search_set,DS_COMB_ARM,fc,xc_incoherent_collapsed_pow,xc_incoherent_collapsed_frq,xc_incoherent_single,xc_incoherent,sp_incoherent,xc,sp,n_comb_xc,n_comb_sp);

    // Calculate the threshold vector
    const uint8 thresh1_n_nines=12;
    double R_th1=chi2cdf_inv(1-pow(10.0,-thresh1_n_nines),2*n_comb_xc*(2*DS_COMB_ARM+1));
    double rx_cutoff=(6*12*15e3/2+4*15e3)/(FS_LTE/16/2);
    vec Z_th1=R_th1*sp_incoherent/rx_cutoff/137/2/n_comb_xc/(2*DS_COMB_ARM+1);

    // Search for the peaks
    if (verbosity>=2) {
      cout << "  Searching for and examining correlation peaks..." << endl;
    }
    peak_search(xc_incoherent_collapsed_pow,xc_incoherent_collapsed_frq,Z_th1,f_search_set,fc,xc_incoherent_single,DS_COMB_ARM,detected_cells);

    // Loop and check each peak
    list<Cell>::iterator iterator=detected_cells.begin();
    while (iterator!=detected_cells.end()) {
      // Detect SSS if possible
      vec sss_h1_np_est_meas;
      vec sss_h2_np_est_meas;
      cvec sss_h1_nrm_est_meas;
      cvec sss_h2_nrm_est_meas;
      cvec sss_h1_ext_est_meas;
      cvec sss_h2_ext_est_meas;
      mat log_lik_nrm;
      mat log_lik_ext;
#define THRESH2_N_SIGMA 3
      (*iterator)=sss_detect((*iterator),capbuf,THRESH2_N_SIGMA,fc,sss_h1_np_est_meas,sss_h2_np_est_meas,sss_h1_nrm_est_meas,sss_h2_nrm_est_meas,sss_h1_ext_est_meas,sss_h2_ext_est_meas,log_lik_nrm,log_lik_ext);
      if ((*iterator).n_id_1==-1) {
        // No SSS detected.
        iterator=detected_cells.erase(iterator);
        continue;
      }
      if (verbosity>=2) {
        cout << "Detected PSS/SSS correspoding to cell ID: " << (*iterator).n_id_cell() << endl;
      }

      // Check to see if this cell has already been detected previously.
      bool match=false;
      {
        boost::mutex::scoped_lock lock(tracked_cell_list.mutex);
        list<tracked_cell_t *>::iterator tci=tracked_cell_list.tracked_cells.begin();
        match=false;
        while (tci!=tracked_cell_list.tracked_cells.end()) {
          if ((*(*tci)).n_id_cell==(*iterator).n_id_cell()) {
            match=true;
            break;
          }
          ++tci;
        }
      }
      if (match) {
        if (verbosity>=2) {
          cout << "Cell already being tracked..." << endl;
        }
        ++iterator;
        continue;
      }

      // Fine FOE
      (*iterator)=pss_sss_foe((*iterator),capbuf,fc);

      // Extract time and frequency grid
      cmat tfg;
      vec tfg_timestamp;
      extract_tfg((*iterator),capbuf,fc,tfg,tfg_timestamp);

      // Create object containing all RS
      RS_DL rs_dl((*iterator).n_id_cell(),6,(*iterator).cp_type);

      // Compensate for time and frequency offsets
      cmat tfg_comp;
      vec tfg_comp_timestamp;
      (*iterator)=tfoec((*iterator),tfg,tfg_timestamp,fc,rs_dl,tfg_comp,tfg_comp_timestamp);

      // Finally, attempt to decode the MIB
      (*iterator)=decode_mib((*iterator),tfg_comp,rs_dl);
      if ((*iterator).n_rb_dl==-1) {
        // No MIB could be successfully decoded.
        iterator=detected_cells.erase(iterator);
        continue;
      }

      if (verbosity>=1) {
        cout << "Detected a new cell!" << endl;
        cout << "  cell ID: " << (*iterator).n_id_cell() << endl;
        cout << "  RX power level: " << db10((*iterator).pss_pow) << " dB" << endl;
        cout << "  residual frequency offset: " << (*iterator).freq_superfine << " Hz" << endl;
        cout << "  frame start: " << (*iterator).frame_start << endl;
      }

      // Launch a cell tracker process!
      k_factor=k_factor;
      //cout << "Timing error is purposely introduced here!!!" << endl;
      //tracked_cell_t * new_cell = new tracked_cell_t((*iterator).n_id_cell(),(*iterator).n_ports,(*iterator).cp_type,(*iterator).frame_start/k_factor+capbuf_sync.late+global_1);
      tracked_cell_t * new_cell = new tracked_cell_t((*iterator).n_id_cell(),(*iterator).n_ports,(*iterator).cp_type,(*iterator).frame_start/k_factor+capbuf_sync.late);
      (*new_cell).thread=boost::thread(tracker_thread,boost::ref(*new_cell),boost::ref(global_thread_data));
      {
        boost::mutex::scoped_lock lock(tracked_cell_list.mutex);
        tracked_cell_list.tracked_cells.push_back(new_cell);
      }
      //cout << "Only one cell is allowed to be detected!!!" << endl;
      //sleep(1000000);

      ++iterator;
    }
  }
  // Will never reach here...
}
