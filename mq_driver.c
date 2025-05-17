#include <linux/init.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/list.h>
#include <linux/moduleparam.h>
#include <linux/string.h>
#include <linux/sched.h>
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Bruno/Thiago/Emanuel");
MODULE_DESCRIPTION("Driver de mensageria simples com comandos /reg, /unr, /msg, /all");
MODULE_VERSION("0.1.0");

#define NAME_SIZE 9

#define DEVICE_NAME "mq"
#define CLASS_NAME  "mq_class"

static int majorNumber;
static int MAX_DEVICES = 8;
static int QUEUE_LEN = 8;
static int CMD_BUF_SIZE = 256;
static struct class* charClass = NULL;
static struct device* charDevice = NULL;

module_param(MAX_DEVICES, int, 0);
module_param(QUEUE_LEN, int, 0);
module_param(CMD_BUF_SIZE, int, 0);
//=================================================================================================
typedef enum { EMPTY, FULL, LIMBO } state_t;

typedef struct message {
    size_t size;
    char *data;    // alocado dinamicamente com a mensagem
    char *sender;  // idem (se desejar guardar sender individualmente)
} message_t;

static char empty_sender[] = "";
static message_t mensagem_null = {
    .size = 0,
    .data = NULL,
    .sender = empty_sender
};

typedef struct message_queue {
    int wp;
    int rp;
    //int capacity;
    state_t state;
    message_t *messages; // vetor contíguo de structs message_t
} message_queue_t;
//=================================================================================================
typedef struct control_block {
    pid_t pid;                         // PID do processo
    char *nome;                   // Nome do processo ou identificador
    message_queue_t *queue;           // Ponteiro para a fila de mensagens do processo
    spinlock_t lock;                  // Proteção para acesso concorrente
    struct control_block *next;       // Próximo na lista
    struct control_block *prev;       // Anterior na lista
} control_block_t;

// Estrutura que representa a lista de control_blocks
typedef struct control_block_list {
    control_block_t *head;  // Ponteiro para o primeiro control_block
    int count;              // Número total de elementos na lista
    spinlock_t lock;        // Exclusão mútua para inserção/remoção de control_blocks
} control_block_list_t;

// Instância global da lista de processos
static control_block_list_t tabela_control_blocks = {
    .head = NULL,
    .count = 0,
    .lock = __SPIN_LOCK_UNLOCKED(tabela_control_blocks.lock)
};
//static DEFINE_SPINLOCK(tabela_control_blocks_lock);
//=================================================================================================
// Prototipos das operações do driver
static int dev_open(struct inode *, struct file *);
static int dev_release(struct inode *, struct file *);
static ssize_t dev_read(struct file *, char *, size_t, loff_t *);
static ssize_t dev_write(struct file *, const char *, size_t, loff_t *);
// Protótipos das funções de utilidade
static int registrar_processo(pid_t pid, const char *nome);
static int remover_processo(pid_t pid);
static control_block_t* buscar_control_block(pid_t pid);
static control_block_t* buscar_control_block_por_nome(const char *nome);
static void inserir_control_block(control_block_t *novo);
static void remover_control_block(control_block_t *cb);
static void atualizar_estado_fila(message_queue_t *q);
static int mq_init_driver(void);
static void mq_exit_driver(void);
//=================================================================================================
// Estrutura de operações do driver
static struct file_operations fops = {
    .owner = THIS_MODULE, // ...
    .open = dev_open,
    .read = dev_read,
    .write = dev_write,
    .release = dev_release,
};

static int mq_init_driver(){

    // Validação dos parâmetros passados via module_param
    if (QUEUE_LEN <= 2 || QUEUE_LEN > 20) {
        printk(KERN_ERR "QUEUE_LEN inválido (%d). Intervalo permitido: 2–20\n", QUEUE_LEN);
        return -EINVAL;
    }
    if (CMD_BUF_SIZE < 16 || CMD_BUF_SIZE > 4096) {
        printk(KERN_ERR "CMD_BUF_SIZE fora do intervalo (%d). Permitido: 16–4096\n", CMD_BUF_SIZE);
        return -EINVAL;
    }
    if (MAX_DEVICES < 1 || MAX_DEVICES > 20) {
        printk(KERN_ERR "MAX_DEVICES inválido (%d). Intervalo permitido: 1–20\n", MAX_DEVICES);
        return -EINVAL;
    }

    printk(KERN_INFO "Carregando o módulo");
    majorNumber = register_chrdev(0, DEVICE_NAME, &fops);
	if (majorNumber < 0) {
		printk(KERN_ALERT "Simple Driver failed to register a major number\n");
		return majorNumber;
	}

    // Register the device class
	charClass = class_create(THIS_MODULE, CLASS_NAME);
	if (IS_ERR(charClass)) {		// Check for error and clean up if there is
		unregister_chrdev(majorNumber, DEVICE_NAME);
		printk(KERN_ALERT "Simple Driver: failed to register device class\n");
		return PTR_ERR(charClass);	// Correct way to return an error on a pointer
	}

    // Register the device driver
	charDevice = device_create(charClass, NULL, MKDEV(majorNumber, 0), NULL, DEVICE_NAME);
	if (IS_ERR(charDevice)) {		// Clean up if there is an error
		class_destroy(charClass);
		unregister_chrdev(majorNumber, DEVICE_NAME);
		printk(KERN_ALERT "Simple Driver: failed to create the device\n");
		return PTR_ERR(charDevice);
	}

    // spin_lock_init(&tabela_control_blocks_lock);
    //spin_lock_init(&tabela_control_blocks.lock);
	//init_driver(MAX_DEVICES, CMD_BUF_SIZE, QUEUE_LEN);
    printk(KERN_INFO "Módulo carregado");
    return 0;
}

static void mq_exit_driver(void){
    spin_lock(&tabela_control_blocks.lock);
    while (tabela_control_blocks.count > 0) {
        control_block_t *cb = tabela_control_blocks.head;
        spin_unlock(&tabela_control_blocks.lock);
        remover_processo(cb->pid); // isso já libera tudo
        spin_lock(&tabela_control_blocks.lock);
    }
    spin_unlock(&tabela_control_blocks.lock);
    device_destroy(charClass, MKDEV(majorNumber, 0));
    class_destroy(charClass);
    unregister_chrdev(majorNumber, DEVICE_NAME);
    printk(KERN_INFO "Módulo descarregado\n");
    return;
}

//=================================================================================================

// Função para buscar um control_block por PID
static control_block_t* buscar_control_block(pid_t pid) {
    control_block_t *curr= tabela_control_blocks.head;
    if (!curr) return NULL;

    do {
        if (curr->pid == pid)
            return curr;
        curr = curr->next;
    } while (curr != tabela_control_blocks.head);

    return NULL; // Não encontrado
}

// Função para buscar um control_block por nome
static control_block_t* buscar_control_block_por_nome(const char *nome) {
    control_block_t *curr = tabela_control_blocks.head;
    if (!curr) return NULL;

    do {
        if (strncmp(curr->nome, nome, NAME_SIZE) == 0) 
            return curr;
        curr = curr->next;
    } while (curr != tabela_control_blocks.head);

    return NULL; // Não encontrado
}

// Função para registrar um processo na lista de control_blocks
static int registrar_processo(pid_t pid, const char *nome) {
    
    control_block_t *novo_cb;
    message_queue_t *fila;
    int i;

    // Start control_block_t ====================================
    novo_cb = kmalloc(sizeof(control_block_t), GFP_KERNEL);
    novo_cb->pid = pid;
    novo_cb->nome = kmalloc_array(NAME_SIZE, sizeof(char), GFP_KERNEL);
    if(tabela_control_blocks.count == MAX_DEVICES -1){
        printk(KERN_WARNING "MAX_DEVICES atingido. Processo PID %d não registrado\n", pid);
        return -ENOENT;
    }
    
    if (!novo_cb) {
        kfree(novo_cb->name);
        return -ENOMEM;
    }
    // Add name ================================================
    
    if (!novo_cb->nome) {
        kfree(novo_cb);
        return -ENOMEM;
    }
    strncpy(novo_cb->nome, nome, NAME_SIZE - 1);
    novo_cb->nome[NAME_SIZE - 1] = '\0';

    // Start message_queue_t ====================================
    // message_queue_t *fila = kmalloc(sizeof(message_queue_t), GFP_KERNEL);
    fila = kmalloc(sizeof(message_queue_t), GFP_KERNEL);
    if (!fila) {
        kfree(novo_cb);
        return -ENOMEM;
    }

    fila->wp = 0;
    fila->rp = 0;
    fila->state = EMPTY;

    // Start message_t ========================================
    fila->messages = kmalloc_array(QUEUE_LEN, sizeof(message_t), GFP_KERNEL);
    if (!fila->messages) {
        kfree(fila);
        kfree(novo_cb->nome);
        kfree(novo_cb);
        return -ENOMEM;
    }
    // Empty the elements ========================================
    for (i = 0; i < QUEUE_LEN; i++) {
        fila->messages[i].data = NULL;
        fila->messages[i].sender = NULL;
        fila->messages[i].size = 0;
    }
    
    novo_cb->queue = fila;
    novo_cb->next = NULL;
    novo_cb->prev = NULL;
    spin_lock_init(&novo_cb->lock);
    inserir_control_block(novo_cb);
    return 0;
}

// Função para inserir um control_block na tabela_control_blocks
static void inserir_control_block(control_block_t *novo) {
    control_block_t *curr, *prev;
    spin_lock(&tabela_control_blocks.lock);
    if(tabela_control_blocks.count == 0){
        tabela_control_blocks.head = novo;
        novo->next = novo;
        novo->prev = novo;
        tabela_control_blocks.count++;
        spin_unlock(&tabela_control_blocks.lock);
        return;
    }
    curr = tabela_control_blocks.head;
    prev = curr->prev;
    prev->next= novo;
    novo->prev = prev;
    novo->next = curr;
    curr->prev = novo;
    tabela_control_blocks.count++;
    spin_unlock(&tabela_control_blocks.lock);
}

// Função para remover de control_blocks por PID
static int remover_processo(pid_t pid) {
    control_block_t *cb = buscar_control_block(pid);
    if (!cb) {
        printk(KERN_WARNING "REMOVE: Processo PID %d não encontrado\n", pid);
        return -ENOENT;
    }
    // Libera a fila de mensagens
    if (cb->queue) {
        int i;
        for (i = QUEUE_LEN-1; i >= 0; i++) {
            kfree(cb->queue->messages[i].data);
            kfree(cb->queue->messages[i].sender); // se for alocado dinamicamente
            kfree(cb->queue->messages[i]);
        }
        kfree(cb->queue);
        //cb->queue = NULL;
    }
    remover_control_block(cb);
    kfree(cb->nome);
    kfree(cb);

    printk(KERN_INFO "REMOVE: Processo PID %d removido com sucesso\n", pid);
    return 0;
}

// Função para remover um control_block da tabela_control_blocks
static void remover_control_block(control_block_t *cb) {
    spin_lock(&tabela_control_blocks.lock);

    if (tabela_control_blocks.count == 1) {
        tabela_control_blocks.head = NULL;
    } else {
        cb->prev->next = cb->next;
        cb->next->prev = cb->prev;
        if (tabela_control_blocks.head == cb) {
            tabela_control_blocks.head = cb->next;
        }
    }

    tabela_control_blocks.count--;

    spin_unlock(&tabela_control_blocks.lock);
}

// Atualiza o estado da fila com base em wp e rp
static void atualizar_estado_fila(message_queue_t *q) {
    int count;
    count = (q->wp >= q->rp) ? (q->wp - q->rp) : (QUEUE_LEN - q->rp + q->wp);
    if (count == 0)
        q->state = EMPTY;
    else if (count == QUEUE_LEN)
        q->state = FULL;
    else
        q->state = LIMBO;
}

//=================================================================================================
static int dev_open(struct inode *inode, struct file *filp) { // Just to generate the descriptor
    return 0;
}
static int dev_release(struct inode *inode, struct file *filp) {
    return 0;
}
// Função open vazia para o driver
static ssize_t dev_read(struct file *filp, char *buffer, size_t len, loff_t *offset) {
    control_block_t *cb;
    message_queue_t *q;
    message_t *msg;
    pid_t pid;
    size_t to_copy;
    pid = current->pid;
    cb = buscar_control_block(pid);
    if (!cb) {
        printk(KERN_WARNING "READ: Processo PID %d não registrado\n", pid);
        return -ENOENT;
    }
    //============================================================================
    spin_lock(&cb->lock);
    q = cb->queue;
    msg = &q->messages[q->rp];

    if (q->state == EMPTY || msg->data == NULL || msg->size == 0) {
        spin_unlock(&cb->lock);
        printk(KERN_INFO "READ: Fila vazia (underflow)\n");
        return -EAGAIN;
    }

    to_copy = (len < msg->size) ? len : msg->size;
    if (copy_to_user(buffer, msg->data, to_copy) != 0) {
        spin_unlock(&cb->lock);
        printk(KERN_WARNING "READ: Falha ao copiar dados para espaço do usuário\n");
        return -EFAULT;
    }

    // Liberar conteúdo da mensagem
    kfree(msg->data);
    kfree(msg->sender); 
    // Resolve Dangling
    msg.data = NULL;
    msg.sender = NULL;
    msg.size = 0;

    q->rp = (q->rp + 1) % QUEUE_LEN;
    atualizar_estado_fila(q);

    spin_unlock(&cb->lock);
    return to_copy;
}

static ssize_t dev_write(struct file *filp, const char *buffer, size_t len, loff_t *offset)
{
    const char *cmd_register, *cmd_unregister, *cmd_message, *cmd_all;
    char buffer2[CMD_BUF_SIZE];
    char cmd_buf[CMD_BUF_SIZE];
    char remetente[NAME_SIZE];
    char destino[NAME_SIZE];
    char nome[NAME_SIZE];
    char code[5];
    control_block_t *cb_pid, *cb_dest, *cb_nome, *cb_origem, *curr;
    message_queue_t *q;
    message_t msg;
    size_t to_copy, sender_len, header;
    bool existe_pid, existe_nome;
    int count, enviados, n;
    pid_t pid;

    memset(destino, 0, sizeof(destino));
    memset(remetente, 0, sizeof(remetente));
    memset(code, 0, sizeof(code));
    memset(nome, 0, sizeof(nome));
    memset(cmd_buf, 0, sizeof(cmd_buf));
    memset(buffer2, 0, sizeof(buffer2));

    to_copy = min(len, (size_t)CMD_BUF_SIZE - 1);//

    if (copy_from_user(cmd_buf, buffer, to_copy) != 0) return -EFAULT;

    cmd_buf[to_copy] = '\0';

    pid = current->pid;
    cmd_register = "/reg ";
    cmd_unregister = "/unr";
    cmd_message = "/msg ";
    cmd_all = "/all ";

    cb_pid = buscar_control_block(pid);
    existe_pid  = (cb_pid != NULL);

    if (strncmp(cmd_buf, cmd_register, strlen(cmd_register)) == 0) {
        n = sscanf(cmd_buf, "%s %7s ", code, nome);
        cb_nome =  buscar_control_block_por_nome(nome);
        existe_nome = ( cb_nome != NULL);
        
        if (existe_pid && existe_nome) {
            printk(KERN_WARNING "WRITE: PID %d e nome \"%s\" já registrados\n", cb_pid->pid, cb_pid->nome);
            return -EEXIST;
        }
        if (existe_pid) {
            printk(KERN_WARNING "WRITE: PID %d já registrado\n", current->pid);
            return -EEXIST;
        }
        if (existe_nome) {
            printk(KERN_WARNING "WRITE: nome \"%s\" já registrado\n", cb_nome->nome);
            return -EEXIST;
        }

        /* registra o novo processo */
        if (registrar_processo(pid, nome) < 0) {
            printk(KERN_ERR "WRITE: falha ao alocar control_block para PID %d\n", current->pid);
            return -ENOMEM;
        }

        printk(KERN_INFO "WRITE: Registrado PID=%d, nome=\"%s\"\n", current->pid, nome);
        return len;
    }
    // /msg ==================================================================
    else if (strncmp(cmd_buf, cmd_message, strlen(cmd_message)) == 0) {
        n = sscanf(cmd_buf, "%s %7s %[^\n]", code, destino, buffer2);
        header = strlen(code) + 1 /*espaço*/ + strlen(destino) + 1 /*espaço*/;
        strncpy(buffer2,cmd_buf + header,strlen(cmd_buf) - header);
        buffer2[strlen(cmd_buf) - header] = '\0';

        cb_origem = buscar_control_block(current->pid);
        if (!cb_origem) {
            printk(KERN_WARNING "WRITE: remetente PID %d não registrado\n", current->pid);
            return -EACCES;
        }
        printk(KERN_WARNING "WRITE: Eu Nome -%s.....\n", cb_origem->nome);

        if (strlen(destino) == 0 || strlen(buffer2) == 0) {
            printk(KERN_WARNING "WRITE: comando /msg mal \"%d\"formatado\"%s\" buffer \"%s\"\n", n, destino, buffer2);
            return -EINVAL;
        }

        cb_dest = buscar_control_block_por_nome(destino);
        if (!cb_dest) {
            printk(KERN_WARNING "WRITE: destinatário \"%s\" não encontrado\n", destino);
            return -ENOENT;
        }
        
        msg.size = strlen(buffer2) + 1;
        msg.data = kmalloc(msg.size, GFP_KERNEL);
        if (!msg.data) return -ENOMEM;

        sender_len = strlen(cb_origem->nome) + 1;
        msg.sender = kmalloc(sender_len, GFP_KERNEL);
        if (!msg.sender) {
            kfree(msg.data);
            return -ENOMEM;
        }

        strncpy(msg.data, buffer2, msg.size);
        strncpy(msg.sender, cb_origem->nome, sender_len);

        spin_lock(&cb_dest->lock);
        q = cb_dest->queue;

        if (q->state == FULL) {
            q->rp = (q->rp + 1) % QUEUE_LEN;
            printk(KERN_WARNING "WRITE: fila cheia, sobrescrevendo mensagem mais antiga de \"%s\"\n", destino);
        }

        q->messages[q->wp] = msg;
        q->wp = (q->wp + 1) % QUEUE_LEN;
        atualizar_estado_fila(q);
        spin_unlock(&cb_dest->lock);

        printk(KERN_INFO "WRITE: mensagem de \"%s\" para \"%s\" enfileirada\n", remetente, destino);
        return len;
    }
    else if (strncmp(cmd_buf, cmd_unregister, strlen(cmd_unregister)) == 0) {
        if (!existe_pid) return -ENOENT;
        remover_processo(pid);
        printk(KERN_INFO "WRITE: Processo PID %d removido\n", pid);
        return len;
    }

    else if (strncmp(cmd_buf, cmd_all, strlen(cmd_all)) == 0) {
        n = sscanf(cmd_buf, "%s %[^\n]", code, buffer2);
        header = strlen(code) + 1 /*espaço*/;
        strncpy(buffer2,cmd_buf + header,strlen(cmd_buf) - header);
        buffer2[strlen(cmd_buf) - header] = '\0';

        cb_origem = buscar_control_block(current->pid);
        if (!cb_origem) {
            printk(KERN_WARNING "WRITE: remetente PID %d não registrado\n", current->pid);
            return -EACCES;
        }

        // Percorre a lista de processos registrados
        spin_lock(&tabela_control_blocks.lock);
        curr = tabela_control_blocks.head;
        count = tabela_control_blocks.count;
        enviados = 0;

        while (count-- > 0 && curr) {
            if (curr->pid != current->pid) {
                message_t m;
                m.size = msg_len;
                m.data = kmalloc(msg_len, GFP_ATOMIC);
                m.sender = kmalloc(sender_len, GFP_ATOMIC);

                if (!m.data || !m.sender) {
                    kfree(m.data);
                    kfree(m.sender);
                    curr = curr->next;
                    continue;
                }

                strncpy(m.data, buffer2, strlen(buffer2));
                strncpy(m.sender, cb_origem->nome, sender_len);

                spin_lock(&curr->lock);
                q = curr->queue;

                if (q->state == FULL) {
                    q->rp = (q->rp + 1) % QUEUE_LEN;
                    printk(KERN_INFO "WRITE: fila cheia, sobrescrevendo em \"%s\"\n", curr->nome);
                }

                q->messages[q->wp] = m;
                q->wp = (q->wp + 1) % QUEUE_LEN;
                atualizar_estado_fila(q);
                spin_unlock(&curr->lock);

                enviados++;
            }
            curr = curr->next;
        }
        spin_unlock(&tabela_control_blocks.lock);

        printk(KERN_INFO "WRITE: mensagem de \"%s\" enviada a %d processos com /all\n", cb_origem->nome, enviados);
        return len;
    }


    printk(KERN_WARNING "WRITE: comando inválido (aguardando /reg, /unr ou /msg)\n");
    return -EINVAL;
}
//======================================================================================
module_init(mq_init_driver);
module_exit(mq_exit_driver);
/*
* long copy_to_user(void __user *to, const void *from, unsigned long n);
* __user ponteiro pro espaco user
*
*
*/
