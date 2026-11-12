/BUSY/ { split($4,a,"="); split($5,b,"="); split($6,c,"="); gsub("us","",a[2]); gsub("us","",b[2]); s+=a[2]; n++; if (b[2]+0>m) m=b[2]+0; o+=c[2]; if (a[2]+0 > wa) wa=a[2]+0 }
/VSYNC/ { split($3,f,"="); split($4,v,"="); nv++; if (nv > 1 && v[2] > 60) { late += v[2] - 60; if (v[2] - 60 > wl) wl = v[2] - 60 } }
END { printf "windows=%d avg_busy=%dus worst_window_avg=%dus worst_frame=%dus frames_over=%d of %d late_vsyncs=%d (worst window %d)\n", n, s/n, wa, m, o, n*60, late, wl }
