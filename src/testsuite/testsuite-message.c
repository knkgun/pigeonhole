/* Copyright (c) 2002-2016 Pigeonhole authors, see the included COPYING file
 */

#include "lib.h"
#include "str.h"
#include "istream.h"
#include "message-address.h"
#include "mail-storage.h"
#include "master-service.h"

#include "sieve-common.h"
#include "sieve-message.h"
#include "sieve-interpreter.h"

#include "sieve-tool.h"

#include "testsuite-common.h"
#include "testsuite-message.h"

/*
 * Testsuite message environment
 */

struct sieve_message_data testsuite_msgdata;

static struct mail *testsuite_mail;

static const char *_default_message_data =
"From: stephan@rename-it.nl\n"
"To: sirius@drunksnipers.com\n"
"Subject: Frop!\n"
"\n"
"Friep!\n";

static string_t *envelope_from;
static string_t *envelope_to;
static string_t *envelope_orig_to;
static string_t *envelope_auth;

pool_t message_pool;

static const char *
testsuite_message_get_address(struct mail *mail, const char *header)
{
	struct message_address *addr;
	const char *str;

	if (mail_get_first_header(mail, header, &str) <= 0)
		return NULL;
	addr = message_address_parse(pool_datastack_create(),
	             (const unsigned char *)str,
	             strlen(str), 1, FALSE);
	return (addr == NULL ||
		addr->mailbox == NULL || *addr->mailbox == '\0' ? NULL :
			( addr->domain == NULL || *addr->domain == '\0' ? addr->mailbox :
				t_strconcat(addr->mailbox, "@", addr->domain, NULL)));
}

static void testsuite_message_set_data(struct mail *mail)
{
	const char *recipient = NULL, *sender = NULL;

	/*
	 * Collect necessary message data
	 */

	/* Get recipient address */
	recipient = testsuite_message_get_address(mail, "Envelope-To");
	if ( recipient == NULL )
		recipient = testsuite_message_get_address(mail, "To");
	if ( recipient == NULL )
		recipient = "recipient@example.com";

	/* Get sender address */
	sender = testsuite_message_get_address(mail, "Return-path");
	if ( sender == NULL )
		sender = testsuite_message_get_address(mail, "Sender");
	if ( sender == NULL )
		sender = testsuite_message_get_address(mail, "From");
	if ( sender == NULL )
		sender = "sender@example.com";

	memset(&testsuite_msgdata, 0, sizeof(testsuite_msgdata));
	testsuite_msgdata.mail = mail;
	testsuite_msgdata.auth_user = sieve_tool_get_username(sieve_tool);

	str_truncate(envelope_from, 0);
	str_append(envelope_from, sender);
	testsuite_msgdata.return_path = str_c(envelope_from);

	str_truncate(envelope_to, 0);
	str_append(envelope_to, recipient);
	testsuite_msgdata.final_envelope_to = str_c(envelope_to);

	str_truncate(envelope_orig_to, 0);
	str_append(envelope_orig_to, recipient);
	testsuite_msgdata.orig_envelope_to = str_c(envelope_orig_to);

	(void)mail_get_first_header(mail, "Message-ID", &testsuite_msgdata.id);
}

void testsuite_message_init(void)
{
	message_pool = pool_alloconly_create("testsuite_message", 6096);

	string_t *default_message = str_new(message_pool, 1024);
	str_append(default_message, _default_message_data);

	testsuite_mail = sieve_tool_open_data_as_mail(sieve_tool, default_message);

	envelope_to = str_new(message_pool, 256);
	envelope_orig_to = str_new(message_pool, 256);
	envelope_from = str_new(message_pool, 256);
	envelope_auth = str_new(message_pool, 256);

	testsuite_message_set_data(testsuite_mail);
}

void testsuite_message_set_string
(const struct sieve_runtime_env *renv, string_t *message)
{
	sieve_message_context_reset(renv->msgctx);

	testsuite_mail = sieve_tool_open_data_as_mail(sieve_tool, message);
	testsuite_message_set_data(testsuite_mail);
}

void testsuite_message_set_file
(const struct sieve_runtime_env *renv, const char *file_path)
{
	sieve_message_context_reset(renv->msgctx);

	testsuite_mail = sieve_tool_open_file_as_mail(sieve_tool, file_path);
	testsuite_message_set_data(testsuite_mail);
}

void testsuite_message_set_mail
(const struct sieve_runtime_env *renv, struct mail *mail)
{
	sieve_message_context_reset(renv->msgctx);

	testsuite_message_set_data(mail);
}

void testsuite_message_deinit(void)
{
	pool_unref(&message_pool);
}

void testsuite_envelope_set_sender
(const struct sieve_runtime_env *renv, const char *value)
{
	sieve_message_context_reset(renv->msgctx);

	str_truncate(envelope_from, 0);

	if ( value != NULL )
		str_append(envelope_from, value);

	testsuite_msgdata.return_path = str_c(envelope_from);
}

void testsuite_envelope_set_recipient
(const struct sieve_runtime_env *renv, const char *value)
{
	sieve_message_context_reset(renv->msgctx);

	str_truncate(envelope_to, 0);

	if ( value != NULL )
		str_append(envelope_to, value);

	testsuite_msgdata.orig_envelope_to = str_c(envelope_to);
	testsuite_msgdata.final_envelope_to = str_c(envelope_to);
}

void testsuite_envelope_set_orig_recipient
(const struct sieve_runtime_env *renv, const char *value)
{
	sieve_message_context_reset(renv->msgctx);

	str_truncate(envelope_orig_to, 0);

	if ( value != NULL )
		str_append(envelope_orig_to, value);

	testsuite_msgdata.orig_envelope_to = str_c(envelope_orig_to);
}

void testsuite_envelope_set_auth_user
(const struct sieve_runtime_env *renv, const char *value)
{
	sieve_message_context_reset(renv->msgctx);

	str_truncate(envelope_auth, 0);

	if ( value != NULL )
		str_append(envelope_auth, value);

	testsuite_msgdata.auth_user = str_c(envelope_auth);
}

