from guppi_utils import *
from astro_utils import current_MJD
from optparse import OptionParser

# Parse command line
par = OptionParser()
par.add_option("-s", "--src", dest="src", help="Set observed source name",
        action="store", default="Fake_PSR")
par.add_option("-r", "--ra", dest="ra", help="Set source R.A. (hh:mm:ss.s)",
        action="store", default="12:34:56.7")
par.add_option("-d", "--dec", dest="dec", help="Set source Dec (+/-dd:mm:ss.s)",
        action="store", default="+12:34:56.7")
par.add_option("-f", "--freq", dest="freq", help="Set center freq (MHz)",
        action="store", type="float", default=1200.0)
par.add_option("-n", "--scannum", dest="scan", help="Set scan number",
        action="store", type="int", default=1);
par.add_option("-l", "--length", dest="len", help="Scan length",
        action="store", type="float", default=3600.0)
par.add_option("-c", "--cal", dest="cal", help="Setup for cal scan",
        action="store_true", default=False)
par.add_option("-g", "--gbt", dest="gbt", 
        help="Use values from gbtstatus (overrides most other settings)",
        action="store_true", default=False)
(opt,arg) = par.parse_args()

g = guppi_status()

g.update("SCANNUM", opt.scan)
g.update("OBS_MODE", "SEARCH")

if (opt.gbt):
    g.update_with_gbtstatus()
    g.update("OBSBW", 800.0)
else:
    g.update("TELESCOP", "GB43m")
    g.update("OBSERVER", "GUPPI Crew")
    g.update("FRONTEND", "None")
    g.update("PROJID", "GUPPI tests")
    g.update("FD_POLN", "LIN")
    g.update("TRK_MODE", "TRACK")
    g.update("SRC_NAME", opt.src)
    g.update("RA_STR", opt.ra)
    g.update("DEC_STR", opt.dec)
    g.update("OBSFREQ", opt.freq)
    g.update("OBSBW", -800.0)

if (opt.cal):
    g.update("SCANLEN", 120.0)
    g.update("BASENAME", "guppi_%s_%04d_cal"%(g['SRC_NAME'], g['SCANNUM']))
    g.update("CAL_MODE", "ON")
else:
    g.update("SCANLEN", opt.len)
    g.update("BASENAME", "guppi_%s_%04d"%(g['SRC_NAME'], g['SCANNUM']))
    g.update("CAL_MODE", "OFF")

g.update("BACKEND", "GUPPI")
g.update("PKTFMT", "GUPPI")
g.update("POL_TYPE", "IQUV")

g.update("CAL_FREQ", 25.0)
g.update("CAL_DCYC", 0.5)
g.update("CAL_PHS", 0.0)

g.update("OBSNCHAN", 2048)
g.update("NPOL", 4)
g.update("NBITS", 8)
g.update("PFB_OVER", 4)
g.update("NBITSADC", 8)
g.update("ACC_LEN", 16)

g.update("NRCVR", 2)

g.update("ONLY_I", 0)
g.update("DS_TIME", 1)
g.update("DS_FREQ", 1)

#g.update("BLOCSIZE", )
g.update("OFFSET0", 0.0)
g.update("SCALE0", 1.0)
g.update("OFFSET1", 0.0)
g.update("SCALE1", 1.0)
g.update("OFFSET2", 0.0)
g.update("SCALE2", 1.0)
g.update("OFFSET3", 0.0)
g.update("SCALE3", 1.0)

g.update("TBIN", abs(g['ACC_LEN']*g['OBSNCHAN']/g['OBSBW']*1e-6))
g.update("CHAN_BW", g['OBSBW']/g['OBSNCHAN'])

if (1):  # in case we don't get a real start time
    MJD = current_MJD()
    MJDd = int(MJD)
    MJDf = MJD - MJDd
    MJDs = int(MJDf * 86400 + 1e-6)
    offs = (MJD - MJDd - MJDs/86400.0) * 86400.0
    g.update("STT_IMJD", MJDd)
    g.update("STT_SMJD", MJDs)
    if offs < 2e-6: offs = 0.0
    g.update("STT_OFFS", offs)

g.update_azza()
g.write()
