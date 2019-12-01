// Simple command-line kernel monitor useful for
// controlling the kernel and exploring the system interactively.

#include <inc/stdio.h>
#include <inc/string.h>
#include <inc/memlayout.h>
#include <inc/assert.h>
#include <inc/x86.h>

#include <kern/console.h>
#include <kern/monitor.h>
#include <kern/kdebug.h>
#include <kern/trap.h>
#include <kern/pmap.h>


#define CMDBUF_SIZE	80	// enough for one VGA text line


struct Command {
	const char *name;
	const char *desc;
	// return -1 to force monitor to exit
	int (*func)(int argc, char** argv, struct Trapframe* tf);
};

static struct Command commands[] = {
	{ "help", "Display this list of commands", mon_help },
	{ "kerninfo", "Display information about the kernel", mon_kerninfo },
	{ "backtrace", "Display backtrace info", mon_backtrace },
	{ "showmappings", "Display mappings info", mon_showmappings },
};

/***** Implementations of basic kernel monitor commands *****/

int
mon_help(int argc, char **argv, struct Trapframe *tf)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(commands); i++)
		cprintf("%s - %s\n", commands[i].name, commands[i].desc);
	return 0;
}

int
mon_kerninfo(int argc, char **argv, struct Trapframe *tf)
{
	extern char _start[], entry[], etext[], edata[], end[];

	cprintf("Special kernel symbols:\n");
	cprintf("  _start                  %08x (phys)\n", _start);
	cprintf("  entry  %08x (virt)  %08x (phys)\n", entry, entry - KERNBASE);
	cprintf("  etext  %08x (virt)  %08x (phys)\n", etext, etext - KERNBASE);
	cprintf("  edata  %08x (virt)  %08x (phys)\n", edata, edata - KERNBASE);
	cprintf("  end    %08x (virt)  %08x (phys)\n", end, end - KERNBASE);
	cprintf("Kernel executable memory footprint: %dKB\n",
		ROUNDUP(end - entry, 1024) / 1024);
	return 0;
}

int
mon_backtrace(int argc, char **argv, struct Trapframe *tf)
{

	// Your code here.
	cprintf("Stack backtrace:\n");
 	// 利用read_ebp() 函数获取当前ebp值
	uint32_t *ebp = (uint32_t *) read_ebp();
	struct Eipdebuginfo info;
	// 利用 ebp 的初始值0判断是否停止
	while(ebp){
		// 利用数组指针运算来获取 eip 以及 args
		cprintf("ebp %08x eip %08x args",ebp,ebp[1]);
		for(int j=2;j<7;++j){
			cprintf(" %08x",ebp[j]);
		}
		cprintf("\n");
		// 调用debuginfo_eip函数
		if (debuginfo_eip(ebp[1], &info) == 0) {
            cprintf("\n     %s:%d: %.*s+%d\n", info.eip_file, info.eip_line, info.eip_fn_namelen, info.eip_fn_name, ebp[1] - info.eip_fn_addr);
        }
		ebp = (uint32_t *)(*ebp);
	}
	return 0;
}

int
mon_showmappings(int argc, char **argv, struct Trapframe *tf){
	if(argc != 3){
		cprintf("Usage:showmappings 0xbegin_addr 0xend_addr\n");
		return -1;
	}
	char *errchar;
	// long int strtol(const char *nptr,char **endptr,int base); 将字符串转为整数，相比atoi会检测转换是否成功，
	// 将非法字符写入endptr，并以 base 为进制。
	uintptr_t start_addr = strtol(argv[1],&errchar,16);
	if(*errchar){
		cprintf("Invalid begin_addr,just need address number which 16base.\n");
		return -1;
	}

	uintptr_t end_addr = strtol(argv[2],&errchar,16);
	if(*errchar){
		cprintf("Invalid end_addr,just need address number which 16base.\n");
		return -1;
	}
	if(start_addr > end_addr){
		cprintf("end_addr must bigger than start_addr\n");
		return -1;
	}

	// 4K对齐
	start_addr = ROUNDUP(start_addr,PGSIZE);
	end_addr = ROUNDUP(end_addr,PGSIZE);

	uintptr_t cur_addr = start_addr;
	while(cur_addr <= end_addr){
		pte_t *pte = pgdir_walk(kern_pgdir,(void*)cur_addr,0);
		// 查找页表，页表未找到或者页表项还没插入(PTE_P==0)
		if(!pte || (*pte & PTE_P)){
			cprintf("virtual address [%08x] - not mapped\n",cur_addr);
		}else{
			cprintf("virtual address [%08x] - physical address [%08x], permission: ",cur_addr,PTE_ADDR(*pte));
			char perm_PS = (*pte &PTE_PS) ? 'S':'-';
			char perm_W = (*pte &PTE_W) ? 'W':'-';
			char perm_U = (*pte &PTE_U) ? 'U':'-';
			cprintf("-%c----%c%cP\n", perm_PS, perm_U, perm_W);
		}
		cur_addr += PGSIZE;
	}
	return 0;
}


/***** Kernel monitor command interpreter *****/

#define WHITESPACE "\t\r\n "
#define MAXARGS 16

static int
runcmd(char *buf, struct Trapframe *tf)
{
	int argc;
	char *argv[MAXARGS];
	int i;

	// Parse the command buffer into whitespace-separated arguments
	argc = 0;
	argv[argc] = 0;
	while (1) {
		// gobble whitespace
		while (*buf && strchr(WHITESPACE, *buf))
			*buf++ = 0;
		if (*buf == 0)
			break;

		// save and scan past next arg
		if (argc == MAXARGS-1) {
			cprintf("Too many arguments (max %d)\n", MAXARGS);
			return 0;
		}
		argv[argc++] = buf;
		while (*buf && !strchr(WHITESPACE, *buf))
			buf++;
	}
	argv[argc] = 0;

	// Lookup and invoke the command
	if (argc == 0)
		return 0;
	for (i = 0; i < ARRAY_SIZE(commands); i++) {
		if (strcmp(argv[0], commands[i].name) == 0)
			return commands[i].func(argc, argv, tf);
	}
	cprintf("Unknown command '%s'\n", argv[0]);
	return 0;
}

void
monitor(struct Trapframe *tf)
{
	char *buf;

	cprintf("Welcome to the JOS kernel monitor!\n");
	cprintf("Type 'help' for a list of commands.\n");

	if (tf != NULL)
		print_trapframe(tf);

	while (1) {
		buf = readline("K> ");
		if (buf != NULL)
			if (runcmd(buf, tf) < 0)
				break;
	}
}
