battery-decision
================

quick mpdecision replacement

Profiles are proritized lexicographically.

Keep in mind that ondemand has to be manually turned on in init.d, it isn't switched on by this software.

Only settings specified in given profile are applied. It's advised to specify all needed settings in all of the profiles.

The idea's to apply profiles dynamically depending on battery and AC status.

**ALWAYS DISABLE MPDECISION** by renaming or *rm -f*'ing it or they'll conflict.
