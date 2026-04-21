#!/usr/bin/env escript
%% Smoke test for the io_uring prim_file write path.
%% Run: escript erts/emulator/nifs/unix/test_uring.escript

main(_) ->
    Dir = "/tmp/uring_smoke_" ++ os:getpid(),
    ok = file:make_dir(Dir),
    try
        test_seq_write_read(Dir),
        test_pwrite_pread(Dir),
        test_iovec(Dir),
        test_concurrent(Dir),
        test_reopen(Dir),
        io:format("ALL PASSED~n")
    catch
        error:{fail, Got, Exp, Label} ->
            io:format("FAIL [~s]: expected ~p, got ~p~n", [Label, Exp, Got]),
            halt(1);
        C:R:S ->
            io:format("FAIL ~p:~p~n~p~n", [C, R, S]),
            halt(1)
    after
        os:cmd("rm -rf " ++ Dir)
    end.

test_seq_write_read(Dir) ->
    {ok, Fd} = prim_file:open(filename:join(Dir,"seq"), [read,write,binary]),
    Data = <<"hello io_uring">>,
    ok = chk(prim_file:write(Fd, Data), ok, "seq write"),
    ok = chk(prim_file:position(Fd, 0), {ok,0}, "seq seek"),
    ok = chk(prim_file:read(Fd, byte_size(Data)), {ok,Data}, "seq read"),
    ok = prim_file:close(Fd).

test_pwrite_pread(Dir) ->
    {ok, Fd} = prim_file:open(filename:join(Dir,"pw"), [read,write,binary]),
    ok = prim_file:write(Fd, <<"aaaa">>),
    ok = chk(prim_file:pwrite(Fd, 2, <<"bb">>), ok, "pwrite"),
    ok = chk(prim_file:pread(Fd, 0, 4), {ok,<<"aabb">>}, "pread"),
    ok = prim_file:close(Fd).

test_iovec(Dir) ->
    Name = filename:join(Dir,"iov"),
    {ok, Fd} = prim_file:open(Name, [write,binary]),
    Chunks = [<<"part1">>, <<":">>, <<"part2">>],
    ok = chk(prim_file:write(Fd, Chunks), ok, "iovec write"),
    ok = prim_file:close(Fd),
    ok = chk(prim_file:read_file(Name), {ok, iolist_to_binary(Chunks)}, "iovec readback").

test_concurrent(Dir) ->
    Self = self(),
    N = 50,
    [spawn(fun() ->
        Name = filename:join(Dir, "c" ++ integer_to_list(I)),
        {ok, Fd} = prim_file:open(Name, [write,binary]),
        ok = prim_file:write(Fd, <<I:64>>),
        ok = prim_file:close(Fd),
        Self ! {done, I}
    end) || I <- lists:seq(1, N)],
    [receive {done,_} -> ok after 5000 -> error(timeout) end || _ <- lists:seq(1,N)],
    [ok = chk(prim_file:read_file(filename:join(Dir,"c"++integer_to_list(I))),
              {ok,<<I:64>>}, "conc "++integer_to_list(I)) || I <- lists:seq(1,N)].

test_reopen(Dir) ->
    Name = filename:join(Dir,"reopen"),
    Data = crypto_rand(4096),
    {ok, Fd} = prim_file:open(Name, [write,binary]),
    ok = prim_file:write(Fd, Data),
    ok = prim_file:close(Fd),
    ok = chk(prim_file:read_file(Name), {ok,Data}, "reopen").

crypto_rand(N) -> list_to_binary([rand:uniform(256)-1 || _ <- lists:seq(1,N)]).

chk(V, V, _) -> ok;
chk(Got, Exp, L) -> error({fail, Got, Exp, L}).
