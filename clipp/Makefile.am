include $(top_srcdir)/build/common.mk
bin_PROGRAMS=clipp

check-local:
	(cd $(srcdir)/tests; abs_builddir=$(abs_builddir) $(ruby19) ./ts_all.rb --verbose)

# Avoid various warnings as error in clipp builds because protobuf is not
# careful about warnings.
#
# NOTE: this must be done in CPPFLAGS vs clipp_CPPFLAGS because
#       of option ordering.
CPPFLAGS += -Wno-shadow -Wno-extra

clipp_SOURCES = \
    clipp.cpp \
    configuration_parser.cpp \
    input.cpp \
    modsec_audit_log.cpp \
    modsec_audit_log_generator.cpp \
    view.cpp \
    ironbee.cpp \
    raw_generator.cpp \
    clipp.pb.cc \
    pb_consumer.cpp \
    htp_consumer.cpp \
    null_consumer.cpp \
    pb_generator.cpp \
    apache_generator.cpp \
    suricata_generator.cpp \
    htp_generator.cpp \
    echo_generator.cpp \
    connection_modifiers.cpp \
    parse_modifier.cpp \
    unparse_modifier.cpp \
    aggregate_modifier.cpp \
    edit_modifier.cpp \
    limit_modifier.cpp \
    select_modifier.cpp \
    header_modifiers.cpp \
    fill_body_modifier.cpp \
    split_modifier.cpp

if HAVE_NIDS
clipp_SOURCES += pcap_generator.cpp
endif

clipp_CFLAGS = @PCRE_CFLAGS@ $(PROTOBUF_CFLAGS) $(NIDS_CFLAGS) $(NET_CFLAGS)
clipp_CPPFLAGS = @PCRE_CPPFLAGS@ $(AM_CPPFLAGS) \
    ${PROTOBUF_CPPFLAGS} \
    $(NIDS_CPPFLAGS) \
    $(NET_CPPFLAGS) \
    $(MODP_CPPFLAGS) \
    $(BOOST_CPPFLAGS)
clipp_LDFLAGS = \
    $(LDFLAGS) \
    $(AM_LDFLAGS) \
    $(PROTOBUF_LDFLAGS) \
    $(BOOST_LDFLAGS) \
    -lboost_system \
    -lboost_filesystem \
    -lboost_program_options \
    -lboost_regex \
    -lboost_thread-mt \
    -lprotobuf

if HAVE_MODP
clipp_LDFLAGS += $(MODP_LDFLAGS) -lmodpbase64
endif

if HAVE_NIDS
clipp_LDFLAGS += $(NIDS_LDFLAGS) $(NET_LDFLAGS) -lnids -lnet -lpcap 
endif

clipp_LDADD = \
    @PCRE_LDADD@ \
    $(top_builddir)/ironbeepp/libibpp.la \
    $(top_builddir)/engine/libironbee.la
