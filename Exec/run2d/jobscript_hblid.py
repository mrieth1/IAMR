from math import floor
import numpy as np
import os

print("n\tHB\tRe")
for n in [0.5, 2.0]:
    for HB in [10, 100]:
        for Re in [1, 100, 500]:
            print(n,"\t",HB,"\t",Re,"\n")
            jobname = "hblid_n%dp%d_HB%d_Re%d" % (int(floor(n)), int(10*(n-floor(n))), HB, Re)
            mu = 1 / (2**((n-1) / 2) * Re * (HB + 1))
            t0 = HB / (Re * (HB + 1))
            run_command = "mpirun -np 2 amr2d.gnu.MPI.ex " +\
                "inputs.2d.lid_driven_cavity " +\
                "ns.dyn_visc_coef=%s " % mu +\
                "ns.yield_stress=%s " % t0 +\
                "ns.flow_index=%s " % n +\
                "amr.plot_file=plt_%s " % jobname +\
                "amr.check_file=chk_%s " % jobname
            os.system(run_command)
