AC_DEFUN([AX_CHECK_OPENSSL],
[
	ac_have_ssl="yes"
	ac_ssl_found="no"
	ac_ssl_include_dir=""
	AC_ARG_ENABLE(openssl, 
		AS_HELP_STRING(--enable-openssl,enables openssl support),
	[
		if test "f$enableval" = "fno"; then
			ac_have_ssl="no"
		elif test "f$enableval" != "fyes"; then
			ac_ssl_include_dir=$enableval
		fi
	])

	if test "f$ac_have_ssl" = "fyes"; then
		ac_ssl_found="no"
		if test "f$ac_ssl_include_dir" != "f"; then
			AC_CHECK_HEADER([$ac_ssl_include_dir/include/openssl/ssl.h], [], 
				AC_MSG_ERROR([openssl not found in $ac_ssl_include_dir]))
			ac_ssl_found="yes"
			OPENSSL_CFLAGS="-I$ac_ssl_include_dir/include"
			OPENSSL_LDFLAGS="-L$ac_ssl_include_dir/lib"
		else
			AC_LANG_SAVE
			AC_LANG[C]
			ac_ssl_path=""
			for dir in /usr /usr/local /opt/ssl; do
				AC_CHECK_HEADER([$dir/include/openssl/ssl.h], [ac_ssl_path=$dir], [])
				if test "f$ac_ssl_path" != "f"; then
					OPENSSL_CFLAGS="-I$ac_ssl_path/include"
					OPENSSL_LDFLAGS="-L$ac_ssl_path/lib"
					break;
				fi
			done
			AC_LANG_RESTORE
		fi
		if test "f$ac_ssl_found" = "fyes"; then
			ifelse([$2], , :, [$2])
			OPENSSL_LIBS="-lssl -lcrypto"
			AC_DEFINE([HAVE_OPENSSL], 1, [Define to 1 if you have openssl installed])
			AC_SUBST(OPENSSL_LIBS)
			AC_SUBST(OPENSSL_CFLAGS)
			AC_SUBST(OPENSSL_LDFLAGS)
		else
			ifelse([$3], , :, [$3])
		fi	
	fi
	AM_CONDITIONAL(HAVE_OPENSSL, [test "f$ac_ssl_found" = "fyes"])
])
