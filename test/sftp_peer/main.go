// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

package main

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"encoding/json"
	"encoding/pem"
	"errors"
	"flag"
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"strconv"
	"sync"
	"time"

	"github.com/pkg/sftp"
	"golang.org/x/crypto/ssh"
)

type peer struct {
	directory string
	state     string
	readOnly  bool
	mutex     sync.Mutex
	execCount int
}

func credentials(state string) (*ssh.ServerConfig, error) {
	_, hostKey, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		return nil, err
	}
	host, err := ssh.NewSignerFromKey(hostKey)
	if err != nil {
		return nil, err
	}
	clientPublic, clientKey, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		return nil, err
	}
	client, err := ssh.NewPublicKey(clientPublic)
	if err != nil {
		return nil, err
	}
	private, err := ssh.MarshalPrivateKey(clientKey, "qsoc-test")
	if err != nil {
		return nil, err
	}
	if err = os.WriteFile(filepath.Join(state, "client_key"), pem.EncodeToMemory(private), 0600); err != nil {
		return nil, err
	}
	if err = os.WriteFile(filepath.Join(state, "host_key.pub"), ssh.MarshalAuthorizedKey(host.PublicKey()), 0600); err != nil {
		return nil, err
	}
	config := &ssh.ServerConfig{
		MaxAuthTries: 1,
		PublicKeyCallback: func(metadata ssh.ConnMetadata, key ssh.PublicKey) (*ssh.Permissions, error) {
			if metadata.User() != "qsoc-test" || !bytes.Equal(key.Marshal(), client.Marshal()) {
				return nil, errors.New("fixture key rejected")
			}
			return &ssh.Permissions{}, nil
		},
	}
	config.AddHostKey(host)
	return config, nil
}

func (p *peer) session(incoming ssh.NewChannel) error {
	if incoming.ChannelType() != "session" {
		return incoming.Reject(ssh.UnknownChannelType, "fixture accepts sessions only")
	}
	channel, requests, err := incoming.Accept()
	if err != nil {
		return err
	}
	defer channel.Close()
	for request := range requests {
		var subsystem struct{ Name string }
		if request.Type != "subsystem" || ssh.Unmarshal(request.Payload, &subsystem) != nil || subsystem.Name != "sftp" {
			if request.Type == "exec" || request.Type == "shell" {
				p.mutex.Lock()
				p.execCount++
				err = os.WriteFile(filepath.Join(p.state, "exec_count"), []byte(strconv.Itoa(p.execCount)), 0600)
				p.mutex.Unlock()
				if err != nil {
					return err
				}
			}
			if err = request.Reply(false, nil); err != nil {
				return err
			}
			continue
		}
		options := []sftp.ServerOption{sftp.WithServerWorkingDirectory(p.directory)}
		if p.readOnly {
			options = append(options, sftp.ReadOnly())
		}
		server, err := sftp.NewServer(channel, options...)
		if err != nil {
			_ = request.Reply(false, nil)
			return err
		}
		defer server.Close()
		if err = request.Reply(true, nil); err != nil {
			return err
		}
		err = server.Serve()
		if errors.Is(err, io.EOF) {
			return nil
		}
		return err
	}
	return nil
}

func (p *peer) connection(raw net.Conn, config *ssh.ServerConfig) {
	defer raw.Close()
	_ = raw.SetDeadline(time.Now().Add(10 * time.Second))
	connection, channels, requests, err := ssh.NewServerConn(raw, config)
	if err != nil {
		return
	}
	defer connection.Close()
	_ = raw.SetDeadline(time.Time{})
	go ssh.DiscardRequests(requests)
	var sessions sync.WaitGroup
	for incoming := range channels {
		sessions.Add(1)
		go func() {
			defer sessions.Done()
			_ = p.session(incoming)
		}()
	}
	sessions.Wait()
}

func run() error {
	p := &peer{}
	flag.StringVar(&p.directory, "root", "", "existing fixture data directory")
	flag.StringVar(&p.state, "state", "", "private fixture state directory")
	flag.BoolVar(&p.readOnly, "read-only", false, "reject SFTP mutations")
	flag.Parse()
	for _, directory := range []string{p.directory, p.state} {
		info, err := os.Stat(directory)
		if err != nil || !info.IsDir() || !filepath.IsAbs(directory) {
			return errors.New("fixture_directory_invalid")
		}
	}
	config, err := credentials(p.state)
	if err != nil {
		return errors.New("fixture_key_generation_failed")
	}
	if err = os.WriteFile(filepath.Join(p.state, "exec_count"), []byte("0"), 0600); err != nil {
		return err
	}
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return err
	}
	defer listener.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Minute)
	defer cancel()
	var active sync.Map
	var connections sync.WaitGroup
	go func() {
		_, _ = io.Copy(io.Discard, os.Stdin)
		cancel()
	}()
	go func() {
		<-ctx.Done()
		_ = listener.Close()
		active.Range(func(key, _ any) bool { _ = key.(net.Conn).Close(); return true })
	}()
	if err = json.NewEncoder(os.Stdout).Encode(map[string]any{"ready": true, "port": listener.Addr().(*net.TCPAddr).Port}); err != nil {
		return err
	}
	for ctx.Err() == nil {
		raw, err := listener.Accept()
		if err != nil {
			if ctx.Err() != nil {
				break
			}
			return err
		}
		active.Store(raw, struct{}{})
		if ctx.Err() != nil {
			_ = raw.Close()
			active.Delete(raw)
			break
		}
		connections.Add(1)
		go func() {
			defer connections.Done()
			defer active.Delete(raw)
			p.connection(raw, config)
		}()
	}
	connections.Wait()
	return nil
}

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "fixture_failed:", err)
		os.Exit(1)
	}
}
