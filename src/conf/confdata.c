/*
 * Copyright (C) 2002 Roman Zippel <zippel@linux-m68k.org>
 * Released under the terms of the GNU GPL v2.0.
 */

/**
 * @file conf/confdata.c
 * @brief GNUnet Setup
 * @author Roman Zippel
 * @author Nils Durner
 */

#include <sys/stat.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#define LKC_DIRECT_LINK
#include "lkc.h"

#include "platform.h"
#include "gnunet_util.h"

const char conf_def_dir[] = "/etc/";
const char conf_def_filename[] = "gnunet.conf";

const char conf_defname[] = "defconfig";

const char *conf_confnames[] = {
				".config",
        "/tmp/.config",
        "/etc/gnunet.conf",
				conf_defname,
				NULL,
};

static char *conf_expand_value(const char *in)
{
	struct symbol *sym;
	const char *src;
	static char res_value[SYMBOL_MAXLENGTH];
	char *dst, name[SYMBOL_MAXLENGTH];

	res_value[0] = 0;
	dst = name;
	while ((src = strchr(in, '$'))) {
		strncat(res_value, in, src - in);
		src++;
		dst = name;
		while (isalnum(*src) || *src == '_')
			*dst++ = *src++;
		*dst = 0;
		sym = sym_lookup(name, "X", 0);
		sym_calc_value(sym);
		strcat(res_value, sym_get_string_value(sym));
		in = src;
	}
	strcat(res_value, in);

	return res_value;
}

char *conf_get_default_confname(void)
{
	struct stat buf;
	static char fullname[PATH_MAX+1];
	char *env, *name;

	name = conf_expand_value(conf_defname);
	env = getenv(SRCTREE);
	if (env) {
		sprintf(fullname, "%s/%s", env, name);
		if (!STAT(fullname, &buf))
			return fullname;
	}
	return name;
}

void extract_setting(char *line, char **setting, char *sect)
{
	int idx = 0;
	while ((!(line[idx] == '!' || line[idx] == 0)) &&
		idx <= 250)
	{
		sect[idx] = line[idx];
		idx++;
	}
	if (! line[idx])
	{
		strcpy(sect, "GENERAL");
		idx = 0;
	}
	else
		sect[idx] = 0;

	if(idx)
		idx++;

	*setting = line + idx;
}

int conf_read(const char *name)
{
	char *val;
	struct symbol *sym;
	struct property *prop;
	struct expr *e;
	int i = 0;
	
	if (!name) {
		const char **names = conf_confnames;
		
		while ((name = *names++)) {
			name = conf_expand_value(name);
			if (cfg_parse_file((char *) name) == 0) {
				printf("#\n"
				       "# using defaults found in %s\n"
				       "#\n", name);
				i = 1;
				break;
			}
		}
	}
	else {
		i = 1;
		cfg_parse_file((char *) name);
	}

	if (!i)
		return 1;
	
	for_all_symbols(i, sym) {
	  sym->flags |= SYMBOL_NEW | SYMBOL_CHANGED;
		sym->flags &= ~SYMBOL_VALID;
		
		val = cfg_get_str(sym->sect, sym->name);
		if (val) {
  		switch (sym->type) {
  			case S_TRISTATE:
  				if (*val == 'm') {
  					sym->user.tri = mod;
  					sym->flags &= ~SYMBOL_NEW;
  					break;
  				}
  			case S_BOOLEAN:
  				sym->user.tri = (*val == 'Y') ? yes : no;
  				sym->flags &= ~SYMBOL_NEW;
  				break;
  			case S_STRING:
  			case S_INT:
  			case S_HEX:
  				if (sym->user.val)
  					free(sym->user.val);
  
  				if (sym_string_valid(sym, val)) {
  					sym->user.val = strdup(val);
  					sym->flags &= ~SYMBOL_NEW;
  				}
  				else {
  					fprintf(stderr, "%s: symbol value '%s' invalid for %s\n", name, val, sym->name);
  					doneParseConfig();
  					exit(1);
  				}

  				if (!sym_string_within_range(sym, val))
  					sym->flags |= SYMBOL_NEW;

  				break;
  			default:
    			sym->user.val = NULL;
    			sym->user.tri = no;
  		}
  		
  		if (sym && sym_is_choice_value(sym)) {
  			struct symbol *cs = prop_get_symbol(sym_get_choice_prop(sym));
  			switch (sym->user.tri) {
  			case no:
  				break;
  			case mod:
  				if (cs->user.tri == yes)
  					/* warn? */;
  				break;
  			case yes:
  				if (cs->user.tri != no)
  					/* warn? */;
  				cs->user.val = sym;
  				break;
  			}
  			cs->user.tri = E_OR(cs->user.tri, sym->user.tri);
  			cs->flags &= ~SYMBOL_NEW;
  		}

  		sym_calc_value(sym);
  		if (sym_has_value(sym) && !sym_is_choice_value(sym)) {
  			if (sym->visible == no)
  				sym->flags |= SYMBOL_NEW;
  		}
  		if (!sym_is_choice(sym))
  			continue;
  		prop = sym_get_choice_prop(sym);
  		for (e = prop->expr; e; e = e->left.expr)
  			if (e->right.sym->visible != no)
  				sym->flags |= e->right.sym->flags & SYMBOL_NEW;
  	}
	}
	
	doneParseConfig();
	
	sym_change_count = 1;

	return 0;
}

int conf_write(const char *name)
{
	FILE *out;
	struct symbol *sym;
	struct menu *menu;
	const char *basename;
	char dirname[128], tmpname[128], dstname[128], newname[128];
	int type;
	const char *str;

	dirname[0] = 0;
	if (name && name[0]) {
		char *slash = strrchr(name, DIR_SEPARATOR);
		if (slash) {
			int size = slash - name + 1;
			memcpy(dirname, name, size);
			dirname[size] = 0;
			if (slash[1])
				basename = slash + 1;
			else
				basename = conf_def_filename;
		} else
			basename = name;
	} else
		basename = conf_def_filename;

	if (! dirname[0])
		strcpy(dirname, conf_def_dir);

	sprintf(newname,
		"%s.tmpconfig.%u",
		dirname,
		(unsigned int) getpid());
	out = FOPEN(newname, "w");
	if (!out)
		return 1;

  fprintf(out, "#%s"
			       "# Automatically generated by gnunet-setup%s"
			       "#%s", NEWLINE, NEWLINE, NEWLINE);

  sym_clear_all_valid();

	menu = rootmenu.list;
	while (menu) {

		sym = menu->sym;
		if (!sym) {

			str = menu_get_prompt(menu);
			if (str && strlen(str) > 0)
				fprintf(out, "%s"
					"#%s"
					"# %s%s"
					"#%s", NEWLINE, NEWLINE, str, NEWLINE, NEWLINE);
			if (menu->section && strlen(menu->section) > 0)
				fprintf(out, "[%s]%s", menu->section, NEWLINE);
		} else if (!(sym->flags & SYMBOL_CHOICE)) {
			sym_calc_value_ext(sym, 1);
			sym->flags &= ~SYMBOL_WRITE;
			type = sym->type;
			if (type == S_TRISTATE) {
				sym_calc_value_ext(modules_sym, 1);
				if (modules_sym->curr.tri == no)
					type = S_BOOLEAN;
			}
			switch (type) {
			case S_BOOLEAN:
			case S_TRISTATE:
				switch (sym_get_tristate_value(sym)) {
				case no:
					fprintf(out, "%s = NO", sym->name);
					break;
				case mod:
					fprintf(out, "%s = m", sym->name);
					break;
				case yes:
					fprintf(out, "%s = YES", sym->name);
					break;
				}
				break;
			case S_STRING:
        fprintf(out, "%s = \"%s\"", sym->name, sym_get_string_value(sym));
				break;
			case S_HEX:
				str = sym_get_string_value(sym);
				if (str[0] != '0' || (str[1] != 'x' && str[1] != 'X')) {
					fprintf(out, "%s = 0x%s", sym->name, str);
					break;
				}
			case S_INT:
				fprintf(out, "%s = %s", sym->name, sym_get_string_value(sym));
				break;
			}
			fprintf(out, "%s", NEWLINE);
		}

		if (menu->list) {
			menu = menu->list;
			continue;
		}
		if (menu->next)
			menu = menu->next;
		else while ((menu = menu->parent)) {
			if (menu->next) {
				menu = menu->next;
				break;
			}
		}
	}
	fclose(out);
	
	sprintf(tmpname, "%s%s.old", dirname, basename);
	UNLINK(tmpname);
	sprintf(dstname, "%s%s", dirname, basename);
	RENAME(dstname, tmpname);

	if (RENAME(newname, dstname))
		return 1;
		
	UNLINK(newname);

	sym_change_count = 0;

	return 0;
}
