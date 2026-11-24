/* jb_tlsN.dll: owns one OS-assigned static TLS index that jackbox.exe lends
 * to a manually mapped image. Built with native TLS (clang) so the image has
 * a TLS directory and the OS loader assigns it a slot. */
static __thread char reserve[16];
extern unsigned long _tls_index;
__declspec(dllexport) unsigned long jb_tls_slot(void) { (void)reserve; return _tls_index; }
